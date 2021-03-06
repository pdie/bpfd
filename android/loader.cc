/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <linux/bpf.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/utsname.h>
#include <linux/elf.h>
#include <unistd.h>

#include "libbpf.h"
#include "utils.h"

#include <iostream>
#include <string>
#include <cstdlib>

#define BPF_FS_PATH "/sys/fs/bpf/"

using namespace std;
using namespace android::bpf;

namespace android {
namespace bpf {

enum code_type {
	TRACEPOINT,
	KPROBE
};

struct code_section {
	enum bpf_prog_type type;
	char *name;
	void *data;
	int data_len;
	void *rel_data;
	int rel_data_len;

	int prog_fd; /* fd after loading */

	/* sections added as discovered */
	struct code_section *next;
};

struct bpf_map_def {
	enum bpf_map_type type;
	unsigned int key_size;
	unsigned int value_size;
	unsigned int max_entries;
	unsigned int map_flags;
	unsigned int inner_map_idx;
	unsigned int numa_node;
};

static int read_elf64_header(const char *elfpath, Elf64_Ehdr *eh)
{
	FILE *elf_file;
	int ret = 0;

	elf_file = fopen(elfpath, "r");
	if (!elf_file) return -1;

	if (fread(eh, sizeof(*eh), 1, elf_file) != 1)
		ret = -1;

	if (elf_file) fclose(elf_file);
	return ret;
}

/* Reads all section header tables into an Shdr array */
static int read_section64_headers_all(const char *elfpath, int *entries, Elf64_Shdr **sh_table_ret)
{
	Elf64_Ehdr eh;
	Elf64_Shdr *sh_table = NULL;
	FILE *elf_file;
	int ret = 0;

	*sh_table_ret = NULL; *entries = 0;

	elf_file = fopen(elfpath, "r");
	if (!elf_file) return -1;

	ret = read_elf64_header(elfpath, &eh);
	if (ret) goto cleanup;

	/* Read offset of shdr table */
	if (fseek(elf_file, eh.e_shoff, SEEK_SET) != 0) {
		ret = -1;
		goto cleanup;
	}

	/* Read shdr table */
	sh_table = (Elf64_Shdr *)malloc(sizeof(*sh_table) * eh.e_shnum);
	if (!sh_table) { 
		ret = -ENOMEM; goto cleanup;
	}

	if (fseek(elf_file, eh.e_shoff, SEEK_SET) != 0) {
		ret = -1; goto cleanup;
	}

	if (fread((void *)sh_table, eh.e_shentsize, eh.e_shnum, elf_file) != eh.e_shnum) {
		ret = -1; goto cleanup;
	}

	*entries = eh.e_shnum;
	*sh_table_ret = sh_table;

cleanup:
	if (elf_file) fclose(elf_file);
	if (ret && sh_table) free(sh_table);

	return ret;
}

/* Read a section by its index - for ex to get sec hdr strtab blob */
static int read_section64_by_id(const char *elfpath, int id, int *bytes, void **section)
{
	Elf64_Shdr *sh_table;
	int entries, ret = 0;
	FILE *elf_file;
	char *sec;

	*section = NULL; *bytes = 0;

	elf_file = fopen(elfpath, "r");
	if (!elf_file) return -1;

	ret = read_section64_headers_all(elfpath, &entries, &sh_table);
	if (ret) goto cleanup;

	sec = (char *)malloc(sh_table[id].sh_size);
	if (!sec) { printf("Memory allocation failure\n"); ret = -ENOMEM; goto cleanup; }

	if (fseek(elf_file, sh_table[id].sh_offset, SEEK_SET) != 0)
		goto cleanup;

	if (fread(sec, sh_table[id].sh_size, 1, elf_file) != 1)
		goto cleanup;

	*bytes = sh_table[id].sh_size;
	*section = sec;

cleanup:
	if (elf_file) fclose(elf_file);
	if (sh_table) free(sh_table);
	if (ret && sec) free(sec);
	return ret;
}

/* Read whole section header string table */
static int read_section64_header_strtab(const char *elfpath, int *bytes, char **strtabp)
{
	Elf64_Ehdr eh;
	FILE *elf_file;
	char *strtab = NULL;
	int ret = 0;

	elf_file = fopen(elfpath, "r");
	if (!elf_file) goto cleanup;

	ret = read_elf64_header(elfpath, &eh);
	if (ret) goto cleanup;

	ret = read_section64_by_id(elfpath, eh.e_shstrndx, bytes, (void **)&strtab);
	if (ret) goto cleanup;

cleanup:
	if (elf_file) fclose(elf_file);

	*strtabp = strtab;
	return ret;
}

/* Get name from offset in strtab */
static int get_sym64_name(const char *elfpath, int name_off, char **name_ret)
{
	char *sec_strtab = NULL, *name, *name2;
	int bytes, ret = 0;

	ret = read_section64_header_strtab(elfpath, &bytes, &sec_strtab);
	if (ret) return ret;

	if (name_off >= bytes) { ret = -1; goto cleanup; }

	name = sec_strtab + name_off;
	name2 = (char *)malloc(strlen(name) + 1);
	memcpy(name2, name, strlen(name) + 1);
	*name_ret = name2;
cleanup:
	if (sec_strtab)
		free(sec_strtab);
	return ret;
}

/* Reads a full section by name - example to get the GPL license */
static int read_section64_by_name(const char *name, const char *elfpath, int *bytes, void **ptr)
{
	char *sec_strtab = NULL;
	char *data = NULL;
	int n_sh_table, ret = 0;
	Elf64_Shdr *sh_table;
	FILE *elf_file;

	elf_file = fopen(elfpath, "r");
	if (!elf_file) return -1;

	ret = read_section64_headers_all(elfpath, &n_sh_table, &sh_table);
	if (ret) goto done;

	ret = read_section64_header_strtab(elfpath, bytes, &sec_strtab);
	if (ret) goto done;

	for(int i = 0; i < n_sh_table; i++) {
		char *secname = sec_strtab + sh_table[i].sh_name;
		if (!secname) continue;

		if (!strcmp(secname, name)) {
			data = (char *)malloc(sh_table[i].sh_size);
			if (fseek(elf_file, sh_table[i].sh_offset, SEEK_SET) != 0) {
				ret = -1; goto done;
			}

			if (fread(data, sh_table[i].sh_size, 1, elf_file) != 1) {
				ret = -1; goto done;
			}
			*bytes = sh_table[i].sh_size;
			*ptr = data;
			goto done;
		}
	}
done:
	if (ret && data) free(data);
	if (sh_table) free(sh_table);
	if (sec_strtab) free(sec_strtab);
	if (elf_file) fclose(elf_file);
	return ret;
}

static int read_section64_by_type(const char *elfpath, int type, int *bytes, void **ptr)
{
	char *data = NULL;
	int n_sh_table, ret = 0;
	Elf64_Shdr *sh_table = NULL;
	FILE *elf_file;

	elf_file = fopen(elfpath, "r");
	if (!elf_file) { return -1; }

	ret = read_section64_headers_all(elfpath, &n_sh_table, &sh_table);
	if (ret) goto done;

	for(int i = 0; i < n_sh_table; i++) {
		if ((int)sh_table[i].sh_type != type)
			continue;

		data = (char *)malloc(sh_table[i].sh_size);
		if (fseek(elf_file, sh_table[i].sh_offset, SEEK_SET) != 0) {
			ret = -1; goto done;
		}
		if (fread(data, sh_table[i].sh_size, 1, elf_file) != 1) {
			ret = -1; goto done;
		}
		*bytes = sh_table[i].sh_size;
		*ptr = data;
		break;
	}

done:
	if (ret && data) free(data);
	if (sh_table) free(sh_table);
	if (elf_file) fclose(elf_file);
	return ret;
}

static int sym64_compare(const void *a1, const void *b1)
{
	Elf64_Sym *a, *b;

	a = (Elf64_Sym *)a1;
	b = (Elf64_Sym *)b1;

	return (a->st_value - b->st_value);
}

static int read_sym64_tab(const char *elfpath, int *bytes, int sort, Elf64_Sym **ptr)
{
	Elf64_Sym *data;
	int ret;

	ret = read_section64_by_type(elfpath, SHT_SYMTAB, bytes, (void **)&data);
	if (ret) return ret;

	if (sort)
		qsort(data, *bytes / sizeof(*data), sizeof(*data), sym64_compare);

	*ptr = data;
	return ret;
}

static int _startswith(const char *a, const char *b)
{
   if (strncmp(a, b, strlen(b)) == 0) return 1;
   return 0;
}

/* Read a section by its index - for ex to get sec hdr strtab blob */
static int read_code_sections(const char *elfpath, struct code_section **cs_ptr)
{
	Elf64_Shdr *sh_table;
	int entries, ret = 0;
	struct code_section *cs_ret = NULL;

	ret = read_section64_headers_all(elfpath, &entries, &sh_table);
	if (ret) return ret;

	for (int i = 0; i < entries; i++) {
		char *name;
		int bytes;
		struct code_section *cs = NULL;

		ret = get_sym64_name(elfpath, sh_table[i].sh_name, &name);
		if (ret) goto done;

		if (name && (_startswith(name, "kprobe/") ||
			     _startswith(name, "tracepoint/"))) {

			cs = (struct code_section *)calloc(1, sizeof(*cs));
			cs->type = (_startswith(name, "kprobe/")) ? BPF_PROG_TYPE_KPROBE : BPF_PROG_TYPE_TRACEPOINT;
			deslash(name);
			cs->name = name;
			ret = read_section64_by_id(elfpath, i, &bytes, &cs->data);
			if (ret) goto done;
			cs->data_len = bytes;
		} else if (name) {
			free(name);
		}

		name = NULL;
		/* Check for rel section */
		if (cs && cs->data && i < entries) {
			ret = get_sym64_name(elfpath, sh_table[i+1].sh_name, &name);
			if (ret) goto done;

			if (name && ((cs->type == BPF_PROG_TYPE_KPROBE && _startswith(name, ".relkprobe/"))||
				     (cs->type == BPF_PROG_TYPE_TRACEPOINT &&_startswith(name, ".reltracepoint/")))) {
				ret = read_section64_by_id(elfpath, i+1, &bytes, &cs->rel_data);
				if (ret) goto done;
				cs->rel_data_len = bytes;
			}
		} else if (name) {
			free(name);
		}

		if (cs) {
			cs->next = cs_ret;
			cs_ret = cs;
		}
	}

	*cs_ptr = cs_ret;
done:
	/* unallocate on errors */
	if (ret && cs_ret) {
		while(cs_ret) {
			struct code_section *cs;
			if (cs_ret->name) free(cs_ret->name);
			if (cs_ret->data) free(cs_ret->data);
			cs = cs_ret;
			cs_ret = cs_ret->next;
			free(cs);
		}
	}
	if (sh_table) free(sh_table);
	return ret;
}

static int get_sym64_name_from_index(const char *elfpath, int index, char **name_ret)
{
	Elf64_Sym *symtab;
	int bytes, ret = 0;
	char *name = NULL;

	ret = read_sym64_tab(elfpath, &bytes, 0 /* !sort */, &symtab);
	if (ret) goto cleanup;

	if (index >= bytes / (int)sizeof(*symtab)) { ret = -1; goto cleanup; }

	ret = get_sym64_name(elfpath, symtab[index].st_name, &name);
	if (ret) goto cleanup;

	*name_ret = name;
cleanup:
	if (symtab) free(symtab);
	return ret;
}

static int get_map_names(const char *elfpath, int *n, char ***map_names_ptr)
{
	Elf64_Sym *symtab = NULL;
	Elf64_Shdr *sh_table = NULL;
	int bytes, entries, maps_idx = -1, nmaps = 0, j = 0, ret = 0;
	char **names;
	char *map_name = NULL;

	ret = read_sym64_tab(elfpath, &bytes, 1 /* sort */, &symtab);
	if (ret) goto cleanup;

	/* Get index of maps section */
	ret = read_section64_headers_all(elfpath, &entries, &sh_table);
	if (ret) goto cleanup;

	for (int i = 0; i < entries; i++) {
		ret = get_sym64_name(elfpath, sh_table[i].sh_name, &map_name);
		if (ret) goto cleanup;

		if (!strncmp(map_name, "maps", 4)) {
			maps_idx = i;
			free(map_name);
			map_name = NULL;
			break;
		}
		if (map_name) { free(map_name); map_name = NULL; }
	}
	if (maps_idx == -1) goto cleanup;

	/* Count number of maps */
	for (int i = 0; i < bytes / (int)sizeof(*symtab); i++)
		if (symtab[i].st_shndx == maps_idx)
			nmaps++;

	names = (char **)calloc(nmaps, sizeof(char *));
	if (!names) { ret = -ENOMEM; goto cleanup; }

	for (int i = 0; i < bytes / (int)sizeof(*symtab); i++) {
		if (symtab[i].st_shndx == maps_idx) {
			ret = get_sym64_name(elfpath, symtab[i].st_name, &names[j++]);
			if (ret) {
				// cleanup all old allocations
				for (int k = 0; k < j; k++)
					if (names[k]) free(names[k]);
				goto cleanup;
			}
		}
	}

	*n = nmaps;
	*map_names_ptr = names;
cleanup:
	if (symtab) free(symtab);
	if (sh_table) free(sh_table);
	if (map_name) free(map_name);
	return ret;
}

static int create_maps(const char *elfpath, int *n, int **map_ret)
{
	int bytes, *map_fds = NULL, ret = 0, nmaps;
	struct bpf_map_def *md = NULL;
	char **map_names = NULL;
	string fname = path_filename(string(elfpath), true);

	ret = read_section64_by_name("maps", elfpath, &bytes, (void **)&md);
	if (ret) goto cleanup;

	ret = get_map_names(elfpath, &nmaps, &map_names);
	if (ret) goto cleanup;

	map_fds = (int *)malloc(*n * sizeof(int));
	if (!map_fds) { ret = -ENOMEM; goto cleanup; }

	for (int i = 0; i < nmaps; i++)
	{
		// Format of pin location is /sys/fs/bpf/map_<filename>_<mapname>
		string map_pin_loc;
		map_pin_loc = string(BPF_FS_PATH) + "map_" + fname + "_"
			      + string(map_names[i]);
		if (access(map_pin_loc.c_str(), F_OK) == 0) {
			ret = -EEXIST;
			goto cleanup;
		}

		int fd = bpf_create_map(md[i].type, map_names[i],
				md[i].key_size, md[i].value_size,
				md[i].max_entries, md[i].map_flags);
		if (fd < 0) {
			ret = fd;
			goto cleanup;
		}

		ret = bpf_obj_pin(fd, map_pin_loc.c_str());
		if (ret < 0)
			goto cleanup;

		map_fds[i] = fd;
	}

	*n = nmaps;
	*map_ret = map_fds;
cleanup:
	if (ret < 0 && map_fds) free(map_fds);
	if (map_names) free(map_names);
	if (md) free(md);
	return ret;
}

static void apply_relo(void *insns_p, Elf64_Addr offset, int fd)
{
	int insn_index;
	struct bpf_insn *insn, *insns;

	insns = (struct bpf_insn *)(insns_p);

	insn_index = offset / sizeof(struct bpf_insn);
	insn = &insns[insn_index];

	if (insn->code != (BPF_LD | BPF_IMM | BPF_DW)) {
		printf("invalid relo for insn %d: code 0x%x\n",
				insn_index, insn->code);
		return;
	}

	insn->imm = fd;
	insn->src_reg = BPF_PSEUDO_MAP_FD;
}

static void apply_map_relocations(const char *elfpath, int *map_fds, struct code_section *cs)
{
	int n_maps, ret;
	char **map_names = NULL;

	ret = get_map_names(elfpath, &n_maps, &map_names);
	if (ret) goto cleanup;

	for (; cs; cs = cs->next) {
		Elf64_Rel *rel = (Elf64_Rel *)(cs->rel_data);
		int n_rel = cs->rel_data_len / sizeof(*rel);

		for (int i = 0; i < n_rel; i++) {
			int sym_index  = ELF64_R_SYM(rel[i].r_info);
			char *sym_name;

			ret = get_sym64_name_from_index(elfpath, sym_index, &sym_name);
			if (ret) {
				if (sym_name) free(sym_name);
				goto cleanup;
			}

			/* Find the map fd and apply relo */
			for (int j = 0; j < n_maps; j++) {
				if (!strcmp(sym_name, map_names[j])) {
					apply_relo(cs->data, rel[i].r_offset, map_fds[j]);
					break;
				}
			}

			if (sym_name) free(sym_name);
		}
	}
cleanup:
	if (map_names) free(map_names);
}

static int load_all_cs(const char *elfpath, struct code_section *cs, char *license)
{
	int ret, fd, kvers;

	if ((kvers = get_machine_kvers()) < 0)
		return -1;

	string fname = path_filename(string(elfpath), true);

	for (; cs; cs = cs->next) {
		string prog_pin_loc;

		switch(cs->type) {
			case BPF_PROG_TYPE_KPROBE:
			case BPF_PROG_TYPE_TRACEPOINT:
				// Format of pin location is
				// /sys/fs/bpf/prog_<filename>_<mapname>
				prog_pin_loc = string(BPF_FS_PATH) + "prog_"
					+ fname + "_" + string(cs->name);
				if (access(prog_pin_loc.c_str(), F_OK) == 0)
					return -EEXIST;

				fd = bpf_prog_load(cs->type, cs->name,
						(struct bpf_insn *)cs->data,
						cs->data_len,
						license, kvers, 0, NULL, 0);
				if (fd <= 0) return -EINVAL;

				ret = bpf_obj_pin(fd, prog_pin_loc.c_str());
				if (ret < 0) return ret;

				cs->prog_fd = fd;
				break;
			default:
				fprintf(stderr, "Undefined cs type %d\n", cs->type);
				return -1;
		}
	}

	return 0;
}

int load_prog(const char *elfpath)
{
	char *license = NULL;
	struct code_section *cs = NULL, *csnext = NULL;
	int n_maps = 0, bytes;
	int *map_fds = NULL, ret = 0;

	ret = read_section64_by_name("license", elfpath, &bytes, (void **)&license);
	if (ret) {
		printf("couldn't find license\n");
		goto cleanup;
	}

	ret = read_code_sections(elfpath, &cs);
	if (ret) {
		printf("couldn't read cs\n");
		goto cleanup;
	}

	ret = create_maps(elfpath, &n_maps, &map_fds);
	if (ret) {
		printf("failed to create maps: %d %d\n", ret, errno);
		goto cleanup;
	}

	apply_map_relocations(elfpath, map_fds, cs);

	ret = load_all_cs(elfpath, cs, license);
	if (ret) {
		printf("failed to load programs\n");
		goto cleanup;
	}

cleanup:
	if (license) free(license);
	if (map_fds) free(map_fds);
	while (cs) {
		csnext = cs->next;
		free(cs);
		cs = csnext;
	}

	return ret;
}

}
}
