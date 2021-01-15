#include "mold.h"

#include "tbb/global_control.h"
#include "tbb/parallel_do.h"
#include "tbb/task_group.h"

#include <fcntl.h>
#include <functional>
#include <iostream>
#include <regex>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_set>

static tbb::task_group parser_tg;
static bool preloading;

static bool is_text_file(MemoryMappedFile *mb) {
  return mb->size() >= 4 &&
         isprint(mb->data()[0]) &&
         isprint(mb->data()[1]) &&
         isprint(mb->data()[2]) &&
         isprint(mb->data()[3]);
}

enum class FileType { UNKNOWN, OBJ, DSO, AR, THIN_AR, TEXT };

static FileType get_file_type(MemoryMappedFile *mb) {
  if (mb->size() >= 20 && memcmp(mb->data(), "\177ELF", 4) == 0) {
    ElfEhdr &ehdr = *(ElfEhdr *)mb->data();
    if (ehdr.e_type == ET_REL)
      return FileType::OBJ;
    if (ehdr.e_type == ET_DYN)
      return FileType::DSO;
    return FileType::UNKNOWN;
  }

  if (mb->size() >= 8 && memcmp(mb->data(), "!<arch>\n", 8) == 0)
    return FileType::AR;
  if (mb->size() >= 8 && memcmp(mb->data(), "!<thin>\n", 8) == 0)
    return FileType::THIN_AR;
  if (is_text_file(mb))
    return FileType::TEXT;
  return FileType::UNKNOWN;
}

static ObjectFile *new_object_file(MemoryMappedFile *mb, std::string archive_name) {
  ObjectFile *file = new ObjectFile(mb, archive_name);
  parser_tg.run([=]() { file->parse(); });
  return file;
}

static SharedFile *new_shared_file(MemoryMappedFile *mb, bool as_needed) {
  SharedFile *file = new SharedFile(mb, as_needed);
  parser_tg.run([=]() { file->parse(); });
  return file;
}

void read_file(MemoryMappedFile *mb, bool as_needed) {
  typedef std::tuple<std::string, u64, u64> Key;
  static std::map<Key, std::vector<ObjectFile *>> cache;

  auto get_key = [](MemoryMappedFile *mb) -> Key {
    return {mb->name, mb->size(), mb->mtime};
  };

  auto lookup = [&](MemoryMappedFile *mb) {
    Key k = get_key(mb);
    std::vector<ObjectFile *> objs = cache[k];
    cache[k].clear();
    return objs;
  };

  switch (get_file_type(mb)) {
  case FileType::OBJ:
    if (preloading) {
      cache[get_key(mb)] = {new_object_file(mb, "")};
      return;
    }

    if (std::vector<ObjectFile *> objs = lookup(mb); !objs.empty())
      out::objs.push_back(objs[0]);
    else
      out::objs.push_back(new_object_file(mb, ""));
    return;
  case FileType::DSO:
    out::dsos.push_back(new_shared_file(mb, as_needed));
    return;
  case FileType::AR:
    if (preloading) {
      for (MemoryMappedFile *child : read_fat_archive_members(mb))
        cache[get_key(mb)].push_back(new_object_file(child, mb->name));
      return;
    }

    if (std::vector<ObjectFile *> objs = lookup(mb); !objs.empty()) {
      append(out::objs, objs);
    } else {
      for (MemoryMappedFile *child : read_archive_members(mb))
        out::objs.push_back(new_object_file(child, mb->name));
    }
    return;
  case FileType::THIN_AR:
    if (preloading) {
      for (MemoryMappedFile *child : read_thin_archive_members(mb))
        cache[get_key(child)].push_back(new_object_file(child, mb->name));
      return;
    }

    for (MemoryMappedFile *child : read_thin_archive_members(mb)) {
      if (std::vector<ObjectFile *> objs = lookup(child); !objs.empty())
        out::objs.push_back(objs[0]);
      else
        out::objs.push_back(new_object_file(child, mb->name));
    }
    return;
  case FileType::TEXT:
    parse_linker_script(mb, as_needed);
    return;
  default:
    Fatal() << mb->name << ": unknown file type";
  }
}

template <typename T>
static std::vector<std::span<T>> split(std::vector<T> &input, int unit) {
  assert(input.size() > 0);
  std::span<T> span(input);
  std::vector<std::span<T>> vec;

  while (span.size() >= unit) {
    vec.push_back(span.subspan(0, unit));
    span = span.subspan(unit);
  }
  if (!span.empty())
    vec.push_back(span);
  return vec;
}

static void resolve_symbols() {
  Timer t("resolve_symbols");

  // Register defined symbols
  tbb::parallel_for_each(out::objs, [](ObjectFile *file) { file->resolve_symbols(); });
  tbb::parallel_for_each(out::dsos, [](SharedFile *file) { file->resolve_symbols(); });

  // Mark reachable objects and DSOs to decide which files to include
  // into an output.
  std::vector<ObjectFile *> root;
  for (ObjectFile *file : out::objs)
    if (file->is_alive)
      root.push_back(file);

  tbb::parallel_do(
    root,
    [&](ObjectFile *file, tbb::parallel_do_feeder<ObjectFile *> &feeder) {
      for (ObjectFile *obj : file->mark_live_objects())
        feeder.add(obj);
    });

  // Eliminate unused archive members and as-needed DSOs.
  erase(out::objs, [](InputFile *file) { return !file->is_alive; });
  erase(out::dsos, [](InputFile *file) { return !file->is_alive; });
}

static void eliminate_comdats() {
  Timer t("comdat");

  tbb::parallel_for_each(out::objs, [](ObjectFile *file) {
    file->resolve_comdat_groups();
  });

  tbb::parallel_for_each(out::objs, [](ObjectFile *file) {
    file->eliminate_duplicate_comdat_groups();
  });
}

static void handle_mergeable_strings() {
  Timer t("resolve_strings");

  // Resolve mergeable string pieces
  tbb::parallel_for_each(out::objs, [](ObjectFile *file) {
    for (MergeableSection *m : file->mergeable_sections) {
      for (StringPiece *piece : m->pieces) {
        MergeableSection *cur = piece->isec;
        while (!cur || cur->file->priority > m->file->priority)
          if (piece->isec.compare_exchange_weak(cur, m))
            break;
      }
    }
  });

  // Calculate the total bytes of mergeable strings for each input section.
  tbb::parallel_for_each(out::objs, [](ObjectFile *file) {
    for (MergeableSection *m : file->mergeable_sections) {
      u32 offset = 0;
      for (StringPiece *piece : m->pieces) {
        if (piece->isec == m && piece->output_offset == -1) {
          piece->output_offset = offset;
          offset += piece->size;
        }
      }
      m->size = offset;
    }
  });

  // Assign each mergeable input section a unique index.
  for (ObjectFile *file : out::objs) {
    for (MergeableSection *m : file->mergeable_sections) {
      m->offset = m->parent.shdr.sh_size;
      m->parent.shdr.sh_size += m->size;
    }
  }
}

// So far, each input section has a pointer to its corresponding
// output section, but there's no reverse edge to get a list of
// input sections from an output section. This function creates it.
//
// An output section may contain millions of input sections.
// So, we append input sections to output sections in parallel.
static void bin_sections() {
  Timer t("bin_sections");

  int unit = (out::objs.size() + 127) / 128;
  std::vector<std::span<ObjectFile *>> slices = split(out::objs, unit);

  int num_osec = OutputSection::instances.size();

  std::vector<std::vector<std::vector<InputChunk *>>> groups(slices.size());
  for (int i = 0; i < groups.size(); i++)
    groups[i].resize(num_osec);

  tbb::parallel_for(0, (int)slices.size(), [&](int i) {
    for (ObjectFile *file : slices[i])
      for (InputSection *isec : file->sections)
        if (isec)
          groups[i][isec->output_section->idx].push_back(isec);
  });

  std::vector<int> sizes(num_osec);

  for (std::span<std::vector<InputChunk *>> group : groups)
    for (int i = 0; i < group.size(); i++)
      sizes[i] += group[i].size();

  tbb::parallel_for(0, num_osec, [&](int j) {
    OutputSection::instances[j]->members.reserve(sizes[j]);
    for (int i = 0; i < groups.size(); i++)
      append(OutputSection::instances[j]->members, groups[i][j]);
  });
}

static void check_duplicate_symbols() {
  Timer t("check_dup_syms");

  tbb::parallel_for_each(out::objs, [&](ObjectFile *file) {
    for (int i = file->first_global; i < file->elf_syms.size(); i++) {
      const ElfSym &esym = file->elf_syms[i];
      Symbol &sym = *file->symbols[i];
      bool is_weak = (esym.st_bind == STB_WEAK);
      bool is_eliminated =
        !esym.is_abs() && !esym.is_common() && !file->sections[esym.st_shndx];

      if (esym.is_defined() && !is_weak && !is_eliminated && sym.file != file)
        Error() << "duplicate symbol: " << *file << ": " << *sym.file
                << ": " << sym.name;
    }
  });

  Error::checkpoint();
}

static void set_isec_offsets() {
  Timer t("isec_offsets");

  tbb::parallel_for_each(OutputSection::instances, [&](OutputSection *osec) {
    if (osec->members.empty())
      return;

    std::vector<std::span<InputChunk *>> slices = split(osec->members, 10000);
    std::vector<u64> size(slices.size());
    std::vector<u32> alignments(slices.size());

    tbb::parallel_for(0, (int)slices.size(), [&](int i) {
      u64 off = 0;
      u32 align = 1;

      for (InputChunk *isec : slices[i]) {
        off = align_to(off, isec->shdr.sh_addralign);
        isec->offset = off;
        off += isec->shdr.sh_size;
        align = std::max<u32>(align, isec->shdr.sh_addralign);
      }

      size[i] = off;
      alignments[i] = align;
    });

    u32 align = *std::max_element(alignments.begin(), alignments.end());

    std::vector<u64> start(slices.size());
    for (int i = 1; i < slices.size(); i++)
      start[i] = align_to(start[i - 1] + size[i - 1], align);

    tbb::parallel_for(1, (int)slices.size(), [&](int i) {
      for (InputChunk *isec : slices[i])
        isec->offset += start[i];
    });

    osec->shdr.sh_size = start.back() + size.back();
    osec->shdr.sh_addralign = align;
  });
}

static void scan_rels() {
  Timer t("scan_rels");

  // Scan relocations to find dynamic symbols.
  tbb::parallel_for_each(out::objs, [&](ObjectFile *file) {
    for (InputSection *isec : file->sections)
      if (isec)
        isec->scan_relocations();
  });

  // Exit if there was a relocation that refers an undefined symbol.
  Error::checkpoint();

  // Aggregate dynamic symbols to a single vector.
  std::vector<InputFile *> files;
  append(files, out::objs);
  append(files, out::dsos);

  std::vector<std::vector<Symbol *>> vec(files.size());

  tbb::parallel_for(0, (int)files.size(), [&](int i) {
    for (Symbol *sym : files[i]->symbols)
      if (sym->flags && sym->file == files[i])
        vec[i].push_back(sym);
  });

  // Assign offsets in additional tables for each dynamic symbol.
  for (Symbol *sym : flatten(vec)) {
    if (sym->is_imported || (sym->flags & NEEDS_DYNSYM))
      out::dynsym->add_symbol(sym);

    if (sym->flags & NEEDS_GOT)
      out::got->add_got_symbol(sym);

    if (sym->flags & NEEDS_PLT)
      out::plt->add_symbol(sym);

    if (sym->flags & NEEDS_GOTTPOFF)
      out::got->add_gottpoff_symbol(sym);

    if (sym->flags & NEEDS_TLSGD)
      out::got->add_tlsgd_symbol(sym);

    if (sym->flags & NEEDS_TLSLD)
      out::got->add_tlsld();

    if (sym->flags & NEEDS_COPYREL) {
      out::copyrel->add_symbol(sym);
      assert(sym->file->is_dso);

      for (Symbol *alias : ((SharedFile *)sym->file)->find_aliases(sym)) {
        if (sym == alias)
          continue;
        alias->has_copyrel = true;
        alias->value = sym->value;
        out::dynsym->add_symbol(alias);
      }
    }
  }
}

static void export_dynamic() {
  Timer t("export_dynamic");

  tbb::parallel_for(0, (int)out::objs.size(), [&](int i) {
    ObjectFile *file = out::objs[i];
    for (Symbol *sym : std::span(file->symbols).subspan(file->first_global))
      if (sym->file == file && config.export_dynamic)
        sym->ver_idx = VER_NDX_GLOBAL;
  });

  for (std::string_view name : config.globals)
    Symbol::intern(name)->ver_idx = VER_NDX_GLOBAL;

  std::vector<std::vector<Symbol *>> vec(out::objs.size());

  tbb::parallel_for(0, (int)out::objs.size(), [&](int i) {
    ObjectFile *file = out::objs[i];
    for (Symbol *sym : std::span(file->symbols).subspan(file->first_global))
      if (sym->file == file && sym->ver_idx != VER_NDX_LOCAL)
        vec[i].push_back(sym);
  });

  for (Symbol *sym : flatten(vec))
    out::dynsym->add_symbol(sym);
}

static void fill_symbol_versions() {
  Timer t("fill_symbol_versions");

  // Create a list of versioned symbols and sort by file and version.
  std::vector<Symbol *> syms = out::dynsym->symbols;
  erase(syms, [](Symbol *sym){ return sym->ver_idx < 2; });

  if (syms.empty())
    return;

  sort(syms, [](Symbol *a, Symbol *b) {
    return std::make_tuple(((SharedFile *)a->file)->soname, a->ver_idx) <
           std::make_tuple(((SharedFile *)b->file)->soname, b->ver_idx);
  });

  // Compute sizes of .gnu.version and .gnu.version_r sections.
  out::versym->contents.resize(out::dynsym->symbols.size() + 1, 1);
  out::versym->contents[0] = 0;

  int sz = sizeof(ElfVerneed) + sizeof(ElfVernaux);
  for (int i = 1; i < syms.size(); i++) {
    if (syms[i - 1]->file != syms[i]->file)
      sz += sizeof(ElfVerneed) + sizeof(ElfVernaux);
    else if (syms[i - 1]->ver_idx != syms[i]->ver_idx)
      sz += sizeof(ElfVernaux);
  }
  out::verneed->contents.resize(sz);

  // Fill .gnu.versoin_r.
  u8 *buf = (u8 *)&out::verneed->contents[0];
  u16 version = 1;
  ElfVerneed *verneed = nullptr;
  ElfVernaux *aux = nullptr;

  auto add_aux = [&](Symbol *sym) {
    SharedFile *file = (SharedFile *)sym->file;
    std::string_view verstr = file->version_strings[sym->ver_idx];

    verneed->vn_cnt++;
    if (aux)
      aux->vna_next = sizeof(ElfVernaux);

    aux = (ElfVernaux *)buf;
    buf += sizeof(*aux);
    aux->vna_hash = elf_hash(verstr);
    aux->vna_other = ++version;
    aux->vna_name = out::dynstr->add_string(verstr);
  };

  auto add_verneed = [&](Symbol *sym) {
    SharedFile *file = (SharedFile *)sym->file;

    out::verneed->shdr.sh_info++;
    if (verneed)
      verneed->vn_next = buf - (u8 *)verneed;

    verneed = (ElfVerneed *)buf;
    buf += sizeof(*verneed);
    verneed->vn_version = 1;
    verneed->vn_file = out::dynstr->find_string(file->soname);
    verneed->vn_aux = sizeof(ElfVerneed);

    aux = nullptr;
    add_aux(sym);
  };

  add_verneed(syms[0]);
  out::versym->contents[syms[0]->dynsym_idx] = version;

  for (int i = 1; i < syms.size(); i++) {
    if (syms[i - 1]->file != syms[i]->file)
      add_verneed(syms[i]);
    else if (syms[i - 1]->ver_idx != syms[i]->ver_idx)
      add_aux(syms[i]);
    out::versym->contents[syms[i]->dynsym_idx] = version;
  }
}

static void clear_padding(u64 filesize) {
  Timer t("clear_padding");

  auto zero = [](OutputChunk *chunk, u64 next_start) {
    u64 pos = chunk->shdr.sh_offset;
    if (chunk->shdr.sh_type != SHT_NOBITS)
      pos += chunk->shdr.sh_size;
    memset(out::buf + pos, 0, next_start - pos);
  };

  for (int i = 1; i < out::chunks.size(); i++)
    zero(out::chunks[i - 1], out::chunks[i]->shdr.sh_offset);
  zero(out::chunks.back(), filesize);
}

// We want to sort output sections in the following order.
//
// alloc readonly data
// alloc readonly code
// alloc writable tdata
// alloc writable tbss
// alloc writable data
// alloc writable bss
// nonalloc
static int get_section_rank(const ElfShdr &shdr) {
  bool alloc = shdr.sh_flags & SHF_ALLOC;
  bool writable = shdr.sh_flags & SHF_WRITE;
  bool exec = shdr.sh_flags & SHF_EXECINSTR;
  bool tls = shdr.sh_flags & SHF_TLS;
  bool nobits = shdr.sh_type == SHT_NOBITS;
  return (!alloc << 5) | (writable << 4) | (exec << 3) | (!tls << 2) | nobits;
}

static u64 set_osec_offsets(std::span<OutputChunk *> chunks) {
  Timer t("osec_offset");

  u64 fileoff = 0;
  u64 vaddr = config.image_base;

  for (OutputChunk *chunk : chunks) {
    if (chunk->starts_new_ptload)
      vaddr = align_to(vaddr, PAGE_SIZE);

    if (vaddr % PAGE_SIZE > fileoff % PAGE_SIZE)
      fileoff += vaddr % PAGE_SIZE - fileoff % PAGE_SIZE;
    else if (vaddr % PAGE_SIZE < fileoff % PAGE_SIZE)
      fileoff = align_to(fileoff, PAGE_SIZE) + vaddr % PAGE_SIZE;

    fileoff = align_to(fileoff, chunk->shdr.sh_addralign);
    vaddr = align_to(vaddr, chunk->shdr.sh_addralign);

    chunk->shdr.sh_offset = fileoff;
    if (chunk->shdr.sh_flags & SHF_ALLOC)
      chunk->shdr.sh_addr = vaddr;

    bool is_bss = chunk->shdr.sh_type == SHT_NOBITS;
    if (!is_bss)
      fileoff += chunk->shdr.sh_size;

    bool is_tbss = is_bss && (chunk->shdr.sh_flags & SHF_TLS);
    if (!is_tbss)
      vaddr += chunk->shdr.sh_size;
  }
  return fileoff;
}

static void fix_synthetic_symbols(std::span<OutputChunk *> chunks) {
  auto start = [](Symbol *sym, OutputChunk *chunk) {
    if (sym) {
      sym->shndx = chunk->shndx;
      sym->value = chunk->shdr.sh_addr;
    }
  };

  auto stop = [](Symbol *sym, OutputChunk *chunk) {
    if (sym) {
      sym->shndx = chunk->shndx;
      sym->value = chunk->shdr.sh_addr + chunk->shdr.sh_size;
    }
  };

  // __bss_start
  for (OutputChunk *chunk : chunks) {
    if (chunk->kind == OutputChunk::REGULAR && chunk->name == ".bss") {
      start(out::__bss_start, chunk);
      break;
    }
  }

  // __ehdr_start
  for (OutputChunk *chunk : chunks) {
    if (chunk->shndx == 1) {
      out::__ehdr_start->shndx = 1;
      out::__ehdr_start->value = out::ehdr->shdr.sh_addr;
      break;
    }
  }

  // __rela_iplt_start and __rela_iplt_end
  start(out::__rela_iplt_start, out::relplt);
  stop(out::__rela_iplt_end, out::relplt);

  // __{init,fini}_array_{start,end}
  for (OutputChunk *chunk : chunks) {
    switch (chunk->shdr.sh_type) {
    case SHT_INIT_ARRAY:
      start(out::__init_array_start, chunk);
      stop(out::__init_array_end, chunk);
      break;
    case SHT_FINI_ARRAY:
      start(out::__fini_array_start, chunk);
      stop(out::__fini_array_end, chunk);
      break;
    }
  }

  // _end, end, _etext, etext, _edata and edata
  for (OutputChunk *chunk : chunks) {
    if (chunk->kind == OutputChunk::HEADER)
      continue;

    if (chunk->shdr.sh_flags & SHF_ALLOC)
      stop(out::_end, chunk);

    if (chunk->shdr.sh_flags & SHF_EXECINSTR)
      stop(out::_etext, chunk);

    if (chunk->shdr.sh_type != SHT_NOBITS && chunk->shdr.sh_flags & SHF_ALLOC)
      stop(out::_edata, chunk);
  }

  // _DYNAMIC
  if (out::dynamic)
    start(out::_DYNAMIC, out::dynamic);

  // _GLOBAL_OFFSET_TABLE_
  if (out::gotplt)
    start(out::_GLOBAL_OFFSET_TABLE_, out::gotplt);

  // __start_ and __stop_ symbols
  for (OutputChunk *chunk : chunks) {
    if (is_c_identifier(chunk->name)) {
      start(Symbol::intern("__start_" + std::string(chunk->name)), chunk);
      stop(Symbol::intern("__stop_" + std::string(chunk->name)), chunk);
    }
  }
}

void cleanup() {
  if (OutputFile::tmpfile)
    unlink(OutputFile::tmpfile);
  if (socket_tmpfile)
    unlink(socket_tmpfile);
}

static void signal_handler(int) {
  cleanup();
  _exit(1);
}

MemoryMappedFile *find_library(std::string name,
                               std::span<std::string_view> lib_paths) {
  for (std::string_view dir : lib_paths) {
    std::string root = dir.starts_with("/") ? config.sysroot : "";
    std::string stem = root + std::string(dir) + "/lib" + name;
    if (!config.is_static)
      if (MemoryMappedFile *mb = MemoryMappedFile::open(stem + ".so"))
        return mb;
    if (MemoryMappedFile *mb = MemoryMappedFile::open(stem + ".a"))
      return mb;
  }
  Fatal() << "library not found: " << name;
}

static std::vector<std::string> add_dashes(std::string name) {
  std::vector<std::string> opts;
  opts.push_back("-" + name);
  if (!name.starts_with("o"))
    opts.push_back("--" + name);
  return opts;
}

static bool read_arg(std::span<std::string_view> &args, std::string_view &arg,
                     std::string name) {
  if (name.size() == 1) {
    if (args[0] == "-" + name) {
      if (args.size() == 1)
        Fatal() << "option -" << name << ": argument missing";
      arg = args[1];
      args = args.subspan(2);
      return true;
    }

    if (args[0].starts_with("-" + name)) {
      arg = args[0].substr(name.size() + 1);
      args = args.subspan(1);
      return true;
    }
    return false;
  }

  for (std::string opt : add_dashes(name)) {
    if (args[0] == opt) {
      if (args.size() == 1)
        Fatal() << "option " << name << ": argument missing";
      arg = args[1];
      args = args.subspan(2);
      return true;
    }

    if (args[0].starts_with(opt + "=")) {
      arg = args[0].substr(opt.size() + 1);
      args = args.subspan(1);
      return true;
    }
  }
  return false;
}

static bool read_flag(std::span<std::string_view> &args, std::string name) {
  for (std::string opt : add_dashes(name)) {
    if (args[0] == opt) {
      args = args.subspan(1);
      return true;
    }
  }
  return false;
}

static bool read_z_flag(std::span<std::string_view> &args, std::string name) {
  if (args.size() >= 2 && args[0] == "-z" && args[1] == name) {
    args = args.subspan(2);
    return true;
  }

  if (!args.empty() && args[0] == "-z" + name) {
    args = args.subspan(1);
    return true;
  }

  return false;
}

static bool read_equal(std::span<std::string_view> &args, std::string_view &arg,
                       std::string name, std::string default_) {
  for (std::string opt : add_dashes(name)) {
    if (args[0] == opt) {
      arg = default_;
      args = args.subspan(1);
      return true;
    }
  }

  for (std::string opt : add_dashes(name)) {
    if (args[0].starts_with(opt + "=")) {
      arg = args[0].substr(opt.size() + 1);
      args = args.subspan(1);
      return true;
    }
  }
  return false;
}

static u64 parse_hex(std::string opt, std::string_view value) {
  if (!value.starts_with("0x") && !value.starts_with("0X"))
    Fatal() << "option -" << opt << ": not a hexadecimal number";
  value = value.substr(2);
  if (value.find_first_not_of("0123456789abcdefABCDEF") != std::string_view::npos)
    Fatal() << "option -" << opt << ": not a hexadecimal number";
  return std::stol(std::string(value), nullptr, 16);
}

static u64 parse_number(std::string opt, std::string_view value) {
  if (value.find_first_not_of("0123456789") != std::string_view::npos)
    Fatal() << "option -" << opt << ": not a number";
  return std::stol(std::string(value));
}

static std::vector<std::string_view> read_response_file(std::string_view path) {
  std::vector<std::string_view> vec;
  MemoryMappedFile *mb = MemoryMappedFile::must_open(std::string(path));

  auto read_quoted = [&](int i, char quote) {
    std::string *buf = new std::string;
    while (i < mb->size() && mb->data()[i] != quote) {
      if (mb->data()[i] == '\\') {
        buf->append(1, mb->data()[i + 1]);
        i += 2;
      } else {
        buf->append(1, mb->data()[i++]);
      }
    }
    if (i >= mb->size())
      Fatal() << path << ": premature end of input";
    vec.push_back(std::string_view(*buf));
    return i + 1;
  };

  auto read_unquoted = [&](int i) {
    std::string *buf = new std::string;
    while (i < mb->size() && !isspace(mb->data()[i]))
      buf->append(1, mb->data()[i++]);
    vec.push_back(std::string_view(*buf));
    return i;
  };

  for (int i = 0; i < mb->size();) {
    if (isspace(mb->data()[i]))
      i++;
    else if (mb->data()[i] == '\'')
      i = read_quoted(i + 1, '\'');
    else if (mb->data()[i] == '\"')
      i = read_quoted(i + 1, '\"');
    else
      i = read_unquoted(i);
  }
  return vec;
}

static std::vector<std::string_view> expand_response_files(char **argv) {
  std::vector<std::string_view> vec;

  for (int i = 0; argv[i]; i++) {
    if (argv[i][0] == '@')
      append(vec, read_response_file(argv[i] + 1));
    else
      vec.push_back(argv[i]);
  }
  return vec;
}

static std::vector<std::string_view> get_input_files(std::span<std::string_view> args) {
  static std::unordered_set<std::string_view> needs_arg({
    "o", "dynamic-linker", "export-dynamic", "e", "entry", "y",
    "trace-symbol", "filler", "sysroot", "thread-count", "z",
    "hash-style", "m", "build-id", "rpath", "version-script",
  });

  std::vector<std::string_view> vec;
  std::vector<std::string> library_paths;

  while (args.empty()) {
    if (needs_arg.contains(args[0])) {
      if (args.size() == 1)
        Fatal() << args[0] << ": missing argument";
      args = args.subspan(2);
      continue;
    }

    std::string_view arg;

    if (read_arg(args, arg, "L") || read_arg(args, arg, "library-path")) {
      library_paths.push_back(std::string(arg));
    }

    if (read_arg(args, arg, "l")) {
      vec.push_back(arg);
      continue;
    }

    if (args[0].starts_with("-")) {
      args = args.subspan(1);
      continue;
    }

    vec.push_back(args[0]);
    args = args.subspan(1);
  }
  return vec;
}

static Config parse_nonpositional_args(std::span<std::string_view> args,
                                       std::vector<std::string_view> &remaining) {
  Config conf;
  conf.thread_count =
    tbb::global_control::active_value(tbb::global_control::max_allowed_parallelism);

  while (!args.empty()) {
    std::string_view arg;

    if (read_arg(args, arg, "o")) {
      conf.output = arg;
    } else if (read_arg(args, arg, "dynamic-linker")) {
      conf.dynamic_linker = arg;
    } else if (read_flag(args, "export-dynamic")) {
      conf.export_dynamic = true;
    } else if (read_arg(args, arg, "e") || read_arg(args, arg, "entry")) {
      conf.entry = arg;
    } else if (read_flag(args, "print-map")) {
      conf.print_map = true;
    } else if (read_flag(args, "stat")) {
      conf.stat = true;
    } else if (read_flag(args, "static")) {
      conf.is_static = true;
    } else if (read_arg(args, arg, "y") || read_arg(args, arg, "trace-symbol")) {
      conf.trace_symbol.push_back(arg);
    } else if (read_arg(args, arg, "filler")) {
      conf.filler = parse_hex("filler", arg);
    } else if (read_arg(args, arg, "L") || read_arg(args, arg, "library-path")) {
      conf.library_paths.push_back(arg);
    } else if (read_arg(args, arg, "sysroot")) {
      conf.sysroot = arg;
    } else if (read_flag(args, "trace")) {
      conf.trace = true;
    } else if (read_flag(args, "pie")) {
      conf.pie = true;
    } else if (read_flag(args, "no-pie")) {
      conf.pie = false;
    } else if (read_flag(args, "relax")) {
      conf.relax = true;
    } else if (read_flag(args, "no-relax")) {
      conf.relax = false;
    } else if (read_flag(args, "perf")) {
      conf.perf = true;
    } else if (read_z_flag(args, "now")) {
      conf.z_now = true;
    } else if (read_flag(args, "no-fork")) {
      conf.fork = false;
    } else if (read_arg(args, arg, "thread-count")) {
      conf.thread_count = parse_number("thread-count", arg);
    } else if (read_flag(args, "discard-all") || read_flag(args, "x")) {
      conf.discard_all = true;
    } else if (read_flag(args, "discard-locals") || read_flag(args, "X")) {
      conf.discard_locals = true;
    } else if (read_flag(args, "strip-all") || read_flag(args, "s")) {
      conf.strip_all = true;
    } else if (read_arg(args, arg, "rpath")) {
      if (!conf.rpaths.empty())
        conf.rpaths += ":";
      conf.rpaths += arg;
    } else if (read_arg(args, arg, "version-script")) {
      conf.version_script.push_back(arg);
    } else if (read_flag(args, "preload")) {
      conf.preload = true;
    } else if (read_arg(args, arg, "z")) {
    } else if (read_arg(args, arg, "hash-style")) {
    } else if (read_arg(args, arg, "m")) {
    } else if (read_equal(args, arg, "build-id", "none")) {
    } else if (read_flag(args, "eh-frame-hdr")) {
    } else if (read_flag(args, "start-group")) {
    } else if (read_flag(args, "end-group")) {
    } else if (read_flag(args, "fatal-warnings")) {
    } else if (read_flag(args, "disable-new-dtags")) {
    } else if (read_flag(args, "as-needed")) {
      remaining.push_back("-as-needed");
    } else if (read_flag(args, "no-as-needed")) {
      remaining.push_back("-no-as-needed");
    } else if (read_arg(args, arg, "l")) {
      remaining.push_back("-l");
      remaining.push_back(arg);
    } else {
      if (args[0][0] == '-')
        Fatal() << "unknown command line option: " << args[0];
      remaining.push_back(args[0]);
      args = args.subspan(1);
    }
  }
  return conf;
}

static void read_input_files(std::span<std::string_view> args) {
  bool as_needed = false;

  while (!args.empty()) {
    std::string_view arg;

    if (read_flag(args, "as-needed")) {
      as_needed = true;
    } else if (read_flag(args, "no-as-needed")) {
      as_needed = false;
    } else if (read_arg(args, arg, "l")) {
      read_file(find_library(std::string(arg), config.library_paths), as_needed);
    } else {
      read_file(MemoryMappedFile::must_open(std::string(args[0])), as_needed);
      args = args.subspan(1);
    }
  }
  parser_tg.wait();
}

static void show_stats() {
  for (ObjectFile *obj : out::objs) {
    static Counter defined("defined_syms");
    defined.inc(obj->first_global - 1);

    static Counter undefined("undefined_syms");
    undefined.inc(obj->symbols.size() - obj->first_global);
  }

  Counter num_input_sections("input_sections");
  for (ObjectFile *file : out::objs)
    num_input_sections.inc(file->sections.size());

  static Counter merged_strings("merged_strings");
  for (MergedSection *osec : MergedSection::instances)
    merged_strings.inc(osec->map.size());

  Counter num_output_chunks("output_out::chunks", out::chunks.size());
  Counter num_objs("num_objs", out::objs.size());
  Counter num_dsos("num_dsos", out::dsos.size());

  Counter::print();
}

int main(int argc, char **argv) {
  Timer t_all("all");

  // Parse non-positional command line options
  std::vector<std::string_view> arg_vector = expand_response_files(argv + 1);
  std::vector<std::string_view> file_args;
  config = parse_nonpositional_args(arg_vector, file_args);

  if (config.output == "")
    Fatal() << "-o option is missing";

  if (!config.preload)
    if (int code; resume_daemon(argv, &code))
      exit(code);

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  // Preload input files
  std::function<void()> on_complete;

  if (config.preload) {
    std::function<void()> wait_for_client;
    daemonize(argv, &wait_for_client, &on_complete);
    preloading = true;
    read_input_files(file_args);
    wait_for_client();
  } else if (config.fork) {
    on_complete = fork_child();
  }

  tbb::global_control tbb_cont(tbb::global_control::max_allowed_parallelism,
                               config.thread_count);

  if (config.stat)
    Counter::enabled = true;
  if (config.pie)
    config.image_base = 0;

  for (std::string_view arg : config.trace_symbol)
    Symbol::intern(arg)->traced = true;

  for (std::string_view arg : config.version_script)
    parse_version_script(std::string(arg));

  // Parse input files
  {
    Timer t("parse");
    preloading = false;
    read_input_files(file_args);
  }

  // Uniquify shared object files with soname
  {
    std::vector<SharedFile *> vec;
    std::unordered_set<std::string_view> seen;
    for (SharedFile *file : out::dsos)
      if (seen.insert(file->soname).second)
        vec.push_back(file);
    out::dsos = vec;
  }

  Timer t_total("total");
  Timer t_before_copy("before_copy");

  out::ehdr = new OutputEhdr;
  out::shdr = new OutputShdr;
  out::phdr = new OutputPhdr;
  out::got = new GotSection;
  out::gotplt = new GotPltSection;
  out::relplt = new RelPltSection;
  out::strtab = new StrtabSection;
  out::shstrtab = new ShstrtabSection;
  out::plt = new PltSection;
  out::symtab = new SymtabSection;
  out::dynsym = new DynsymSection;
  out::dynstr = new DynstrSection;
  out::copyrel = new CopyrelSection;

  if (!config.is_static) {
    out::interp = new InterpSection;
    out::dynamic = new DynamicSection;
    out::reldyn = new RelDynSection;
    out::hash = new HashSection;
    out::versym = new VersymSection;
    out::verneed = new VerneedSection;
  }

  out::chunks.push_back(out::got);
  out::chunks.push_back(out::plt);
  out::chunks.push_back(out::gotplt);
  out::chunks.push_back(out::relplt);
  out::chunks.push_back(out::reldyn);
  out::chunks.push_back(out::dynamic);
  out::chunks.push_back(out::dynsym);
  out::chunks.push_back(out::dynstr);
  out::chunks.push_back(out::shstrtab);
  out::chunks.push_back(out::symtab);
  out::chunks.push_back(out::strtab);
  out::chunks.push_back(out::hash);
  out::chunks.push_back(out::copyrel);
  out::chunks.push_back(out::versym);
  out::chunks.push_back(out::verneed);

  // Set priorities to files. File priority 1 is reserved for the internal file.
  int priority = 2;
  for (ObjectFile *file : out::objs)
    if (!file->is_in_archive)
      file->priority = priority++;
  for (ObjectFile *file : out::objs)
    if (file->is_in_archive)
      file->priority = priority++;
  for (SharedFile *file : out::dsos)
    file->priority = priority++;

  // Resolve symbols and fix the set of object files that are
  // included to the final output.
  resolve_symbols();

  if (config.trace) {
    for (ObjectFile *file : out::objs)
      SyncOut() << *file;
    for (SharedFile *file : out::dsos)
      SyncOut() << *file;
  }

  // Remove redundant comdat sections (e.g. duplicate inline functions).
  eliminate_comdats();

  // Merge strings constants in SHF_MERGE sections.
  handle_mergeable_strings();

  // Create .bss sections for common symbols.
  {
    Timer t("common");
    tbb::parallel_for_each(out::objs, [](ObjectFile *file) {
      file->convert_common_symbols();
    });
  }

  // Bin input sections into output sections
  bin_sections();

  // Assign offsets within an output section to input sections.
  set_isec_offsets();

  // Sections are added to the section lists in an arbitrary order because
  // they are created in parallel. Sort them to to make the output deterministic.
  auto section_compare = [](OutputChunk *x, OutputChunk *y) {
    return std::make_tuple(x->name, (u32)x->shdr.sh_type, (u64)x->shdr.sh_flags) <
           std::make_tuple(y->name, (u32)y->shdr.sh_type, (u64)y->shdr.sh_flags);
  };

  sort(OutputSection::instances, section_compare);
  sort(MergedSection::instances, section_compare);

  // Add sections to the section lists
  for (OutputSection *osec : OutputSection::instances)
    if (osec->shdr.sh_size)
      out::chunks.push_back(osec);
  for (MergedSection *osec : MergedSection::instances)
    if (osec->shdr.sh_size)
      out::chunks.push_back(osec);

  erase(out::chunks, [](OutputChunk *c) { return !c; });

  // Sort the sections by section flags so that we'll have to create
  // as few segments as possible.
  sort(out::chunks, [](OutputChunk *a, OutputChunk *b) {
    return get_section_rank(a->shdr) < get_section_rank(b->shdr);
  });

  // Create a dummy file containing linker-synthesized symbols
  // (e.g. `__bss_start`).
  out::internal_file = new ObjectFile;
  out::internal_file->resolve_symbols();
  out::objs.push_back(out::internal_file);

  // Convert weak symbols to absolute symbols with value 0.
  {
    Timer t("undef_weak");
    tbb::parallel_for_each(out::objs, [](ObjectFile *file) {
      file->handle_undefined_weak_symbols();
    });
  }

  // Beyond this point, no new symbols will be added to the result.

  // Copy shared object name strings to .dynstr.
  for (SharedFile *file : out::dsos)
    out::dynstr->add_string(file->soname);

  // Copy DT_RUNPATH strings to .dynstr.
  out::dynstr->add_string(config.rpaths);

  // Add headers and sections that have to be at the beginning
  // or the ending of a file.
  out::chunks.insert(out::chunks.begin(), out::ehdr);
  out::chunks.insert(out::chunks.begin() + 1, out::phdr);
  if (out::interp)
    out::chunks.insert(out::chunks.begin() + 2, out::interp);
  out::chunks.push_back(out::shdr);

  // Make sure that all symbols have been resolved.
  check_duplicate_symbols();

  // Scan relocations to find symbols that need entries in .got, .plt,
  // .got.plt, .dynsym, .dynstr, etc.
  scan_rels();

  // Put symbols to .dynsym.
  export_dynamic();

  // Sort .dynsym contents.
  out::dynsym->sort_symbols();

  // Fill .gnu.version and .gnu.version_r section contents.
  fill_symbol_versions();

  // Compute .symtab and .strtab sizes for each file.
  tbb::parallel_for_each(out::objs, [](ObjectFile *file) {
    file->compute_symtab();
  });

  // Now that we have computed sizes for all sections and assigned
  // section indices to them, so we can fix section header contents
  // for all output sections.
  for (OutputChunk *chunk : out::chunks)
    chunk->update_shdr();

  erase(out::chunks, [](OutputChunk *c) { return c->shdr.sh_size == 0; });

  // Set section indices.
  for (int i = 0, shndx = 1; i < out::chunks.size(); i++)
    if (out::chunks[i]->kind != OutputChunk::HEADER)
      out::chunks[i]->shndx = shndx++;

  for (OutputChunk *chunk : out::chunks)
    chunk->update_shdr();

  // Assign offsets to output sections
  u64 filesize = set_osec_offsets(out::chunks);

  // Fix linker-synthesized symbol addresses.
  fix_synthetic_symbols(out::chunks);

  // At this point, file layout is fixed. Beyond this, you can assume
  // that symbol addresses including their GOT/PLT/etc addresses have
  // a correct final value.

  // Some types of relocations for TLS symbols need the TLS segment
  // address. Find it out now.
  for (ElfPhdr phdr : create_phdr()) {
    if (phdr.p_type == PT_TLS) {
      out::tls_begin = phdr.p_vaddr;
      out::tls_end = align_to(phdr.p_vaddr + phdr.p_memsz, phdr.p_align);
    }
  }

  t_before_copy.stop();

  // Create an output file
  OutputFile *file = OutputFile::open(config.output, filesize);
  out::buf = file->buf;

  Timer t_copy("copy");

  // Copy input sections to the output file
  {
    Timer t("copy_buf");
    tbb::parallel_for_each(out::chunks, [&](OutputChunk *chunk) {
      chunk->copy_buf();
    });
    Error::checkpoint();
  }

  // Zero-clear paddings between sections
  clear_padding(filesize);

  // Commit
  file->close();

  t_copy.stop();
  t_total.stop();
  t_all.stop();

  if (config.print_map)
    print_map();

  // Show stats numbers
  if (Counter::enabled)
    show_stats();

  if (config.perf)
    Timer::print();

  std::cout << std::flush;
  std::cerr << std::flush;
  if (on_complete)
    on_complete();
  std::quick_exit(0);
}
