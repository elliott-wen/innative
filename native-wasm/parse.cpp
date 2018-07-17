// Copyright �2018 Black Sphere Studios
// For conditions of distribution and use, see copyright notice in native-wasm.h

#include "parse.h"
#include "validate.h"
#include <malloc.h>
#include <assert.h>
#include <algorithm>

__KHASH_IMPL(exports, kh_inline, kh_cstr_t, varuint32, 1, kh_str_hash_func, kh_str_hash_equal);
__KHASH_IMPL(modules, kh_inline, kh_cstr_t, varuint32, 1, kh_str_hash_func, kh_str_hash_equal);

NW_FORCEINLINE ERROR_CODE ParseVarUInt32(Stream& s, varuint32& target) { ERROR_CODE err; target = static_cast<varuint32>(DecodeLEB128(s, err, 32, false)); return err; }
NW_FORCEINLINE ERROR_CODE ParseVarSInt7(Stream& s, varsint7& target) { ERROR_CODE err; target = static_cast<varsint7>(DecodeLEB128(s, err, 7, true)); return err; }
NW_FORCEINLINE ERROR_CODE ParseVarUInt7(Stream& s, varuint7& target) { ERROR_CODE err; target = static_cast<varuint7>(DecodeLEB128(s, err, 7, false)); return err; }
NW_FORCEINLINE ERROR_CODE ParseVarUInt1(Stream& s, varuint1& target) { ERROR_CODE err; target = static_cast<varuint1>(DecodeLEB128(s, err, 1, false)); return err; }
NW_FORCEINLINE ERROR_CODE ParseByte(Stream& s, byte& target) { return !s.Read(target) ? ERR_PARSE_UNEXPECTED_EOF : ERR_SUCCESS; }

template<class T, typename... Args>
struct Parse
{
  template<ERROR_CODE(*PARSE)(Stream&, T&, Args...)>
  static ERROR_CODE Array(Stream& s, T*& ptr, varuint32& size, Args... args)
  {
    ERROR_CODE err = ParseVarUInt32(s, size);
    if(err < 0)
      return err;

    if(!size)
    {
      ptr = 0;
      return err;
    }

    T* r = tmalloc<T>(size);
    if(!r)
      return ERR_FATAL_OUT_OF_MEMORY;

    for(varuint32 i = 0; i < size && err >= 0; ++i)
      err = PARSE(s, r[i], args...);
    ptr = r;

    return err;
  }
};

ERROR_CODE ParseByteArray(Stream& s, ByteArray& section, bool nullterminate)
{
  ERROR_CODE err = ParseVarUInt32(s, section.n_bytes);
  if(err < 0)
    return err;

  section.bytes = 0;
  if(section.n_bytes > 0)
  {
    section.bytes = tmalloc<uint8_t>(section.n_bytes + (nullterminate ? 1 : 0));
    if(!section.bytes)
      return ERR_FATAL_OUT_OF_MEMORY;

    uint32 bytes = static_cast<uint32>(s.ReadBytes(section.bytes, section.n_bytes));
    if(bytes != section.n_bytes)
      return ERR_PARSE_UNEXPECTED_EOF;

    if(nullterminate)
      section.bytes[section.n_bytes] = 0;
  }

  return ERR_SUCCESS;
}

ERROR_CODE ParseIdentifier(Stream& s, ByteArray& section)
{
  return ParseByteArray(s, section, true); // For identifiers, we allocate one extra null terminator byte that isn't included in the size
}

ERROR_CODE ParseInitializer(Stream& s, Instruction& ins)
{
  ERROR_CODE err = ParseInstruction(s, ins);
  Instruction end;

  if(err >= 0)
    err = ParseInstruction(s, end);

  if(err >= 0 && end.opcode != OP_end)
    err = ERR_FATAL_EXPECTED_END_INSTRUCTION;

  return err;
}

ERROR_CODE ParseFuncSig(Stream& s, FunctionSig& sig)
{
  ERROR_CODE err = ParseVarSInt7(s, sig.form);
  if(err < 0)
    return err;

  if(sig.form == TE_func)
  {
    err = Parse<varsint7>::template Array<&ParseVarSInt7>(s, sig.params, sig.n_params);

    if(err >= 0)
      err = Parse<varsint7>::template Array<&ParseVarSInt7>(s, sig.returns, sig.n_returns);
  }
  else
    err = ERR_FATAL_UNKNOWN_FUNCTION_SIGNATURE;

  return err;
}

ERROR_CODE ParseResizableLimits(Stream& s, ResizableLimits& limits)
{
  ERROR_CODE err = ParseVarUInt32(s, limits.flags);

  if(err >= 0)
    err = ParseVarUInt32(s, limits.minimum);

  if(err >= 0 && (limits.flags & 0x1) != 0)
    err = ParseVarUInt32(s, limits.maximum);

  return err;
}
ERROR_CODE ParseMemoryDesc(Stream& s, MemoryDesc& mem)
{
  return ParseResizableLimits(s, mem.limits);
}

ERROR_CODE ParseTableDesc(Stream& s, TableDesc& t)
{
  ERROR_CODE err = ParseVarSInt7(s, t.element_type);

  if(err >= 0)
    err = ParseResizableLimits(s, t.resizable);

  return err;
}

ERROR_CODE ParseGlobalDesc(Stream& s, GlobalDesc& g)
{
  ERROR_CODE err = ParseVarSInt7(s, g.type);

  if(err >= 0)
    err = ParseVarUInt1(s, g.mutability);

  return err;
}

ERROR_CODE ParseGlobalDecl(Stream& s, GlobalDecl& g)
{
  ERROR_CODE err = ParseGlobalDesc(s, g.desc);

  if(err >= 0)
    err = ParseInitializer(s, g.init);

  return err;
}

ERROR_CODE ParseImport(Stream& s, Import& i)
{
  ERROR_CODE err = ParseIdentifier(s, i.module_name);

  if(err >= 0)
    err = ParseIdentifier(s, i.export_name);

  if(err >= 0)
    err = ParseVarUInt7(s, i.kind);

  if(err >= 0)
  {
    switch(i.kind)
    {
    case KIND_FUNCTION:
      i.func_desc.debug_name.bytes = nullptr;
      i.func_desc.param_names = 0;
      return ParseVarUInt32(s, i.func_desc.sig_index);
    case KIND_TABLE:
      return ParseTableDesc(s, i.table_desc);
    case KIND_MEMORY:
      return ParseMemoryDesc(s, i.mem_desc);
    case KIND_GLOBAL:
      return ParseGlobalDesc(s, i.global_desc);
    default:
      err = ERR_FATAL_UNKNOWN_KIND;
    }
  }

  return err;
}

ERROR_CODE ParseExport(Stream& s, Export& e)
{
  ERROR_CODE err = ParseIdentifier(s, e.name);

  if(err >= 0)
    err = ParseVarUInt7(s, e.kind);

  if(err >= 0)
    err = ParseVarUInt32(s, e.index);

  return err;
}

ERROR_CODE ParseInstruction(Stream& s, Instruction& ins)
{
  ERROR_CODE err = ParseByte(s, ins.opcode);
  if(err < 0)
    return err;

  switch(ins.opcode)
  {
  case OP_block:
  case OP_loop:
  case OP_if:
    ins.immediates[0]._varuint7 = ReadVarUInt7(s, err);
    break;
  case OP_br:
  case OP_br_if:
  case OP_get_local:
  case OP_set_local:
  case OP_tee_local:
  case OP_get_global:
  case OP_set_global:
  case OP_call:
    ins.immediates[0]._varuint32 = ReadVarUInt32(s, err);
    break;
  case OP_i32_const:
    ins.immediates[0]._varsint32 = ReadVarInt32(s, err);
    break;
  case OP_i64_const:
    ins.immediates[0]._varsint64 = ReadVarInt64(s, err);
    break;
  case OP_f32_const:
    ins.immediates[0]._float32 = ReadFloat32(s, err);
    break;
  case OP_f64_const:
    ins.immediates[0]._float64 = ReadFloat64(s, err);
    break;
  case OP_memory_grow:
  case OP_memory_size:
    ins.immediates[0]._varuint1 = ReadVarUInt1(s, err);
    break;
  case OP_br_table:
    err = Parse<varuint32>::template Array<&ParseVarUInt32>(s, ins.immediates[0].table, ins.immediates[0].n_table);

    if(err >= 0)
      ins.immediates[1]._varuint32 = ReadVarUInt32(s, err);

    break;
  case OP_call_indirect:
    ins.immediates[0]._varuint32 = ReadVarUInt32(s, err);

    if(err >= 0)
      ins.immediates[1]._varuint1 = ReadVarUInt1(s, err);

    break;
  case OP_i32_load:
  case OP_i64_load:
  case OP_f32_load:
  case OP_f64_load:
  case OP_i32_store:
  case OP_i64_store:
  case OP_f32_store:
  case OP_f64_store:
  case OP_i32_load8_s:
  case OP_i32_load16_s:
  case OP_i64_load8_s:
  case OP_i64_load16_s:
  case OP_i64_load32_s:
  case OP_i32_load8_u:
  case OP_i32_load16_u:
  case OP_i64_load8_u:
  case OP_i64_load16_u:
  case OP_i64_load32_u:
  case OP_i32_store8:
  case OP_i32_store16:
  case OP_i64_store8:
  case OP_i64_store16:
  case OP_i64_store32:
    ins.immediates[0]._memflags = ReadVarUInt32(s, err);

    if(err >= 0)
      ins.immediates[1]._varuptr = ReadVarUInt64(s, err);

    break;
  case OP_unreachable:
  case OP_nop:
  case OP_else:
  case OP_end:
  case OP_return:
  case OP_drop:
  case OP_select:
  case OP_i32_eqz:
  case OP_i32_eq:
  case OP_i32_ne:
  case OP_i32_lt_s:
  case OP_i32_lt_u:
  case OP_i32_gt_s:
  case OP_i32_gt_u:
  case OP_i32_le_s:
  case OP_i32_le_u:
  case OP_i32_ge_s:
  case OP_i32_ge_u:
  case OP_i64_eqz:
  case OP_i64_eq:
  case OP_i64_ne:
  case OP_i64_lt_s:
  case OP_i64_lt_u:
  case OP_i64_gt_s:
  case OP_i64_gt_u:
  case OP_i64_le_s:
  case OP_i64_le_u:
  case OP_i64_ge_s:
  case OP_i64_ge_u:
  case OP_f32_eq:
  case OP_f32_ne:
  case OP_f32_lt:
  case OP_f32_gt:
  case OP_f32_le:
  case OP_f32_ge:
  case OP_f64_eq:
  case OP_f64_ne:
  case OP_f64_lt:
  case OP_f64_gt:
  case OP_f64_le:
  case OP_f64_ge:
  case OP_i32_clz:
  case OP_i32_ctz:
  case OP_i32_popcnt:
  case OP_i32_add:
  case OP_i32_sub:
  case OP_i32_mul:
  case OP_i32_div_s:
  case OP_i32_div_u:
  case OP_i32_rem_s:
  case OP_i32_rem_u:
  case OP_i32_and:
  case OP_i32_or:
  case OP_i32_xor:
  case OP_i32_shl:
  case OP_i32_shr_s:
  case OP_i32_shr_u:
  case OP_i32_rotl:
  case OP_i32_rotr:
  case OP_i64_clz:
  case OP_i64_ctz:
  case OP_i64_popcnt:
  case OP_i64_add:
  case OP_i64_sub:
  case OP_i64_mul:
  case OP_i64_div_s:
  case OP_i64_div_u:
  case OP_i64_rem_s:
  case OP_i64_rem_u:
  case OP_i64_and:
  case OP_i64_or:
  case OP_i64_xor:
  case OP_i64_shl:
  case OP_i64_shr_s:
  case OP_i64_shr_u:
  case OP_i64_rotl:
  case OP_i64_rotr:
  case OP_f32_abs:
  case OP_f32_neg:
  case OP_f32_ceil:
  case OP_f32_floor:
  case OP_f32_trunc:
  case OP_f32_nearest:
  case OP_f32_sqrt:
  case OP_f32_add:
  case OP_f32_sub:
  case OP_f32_mul:
  case OP_f32_div:
  case OP_f32_min:
  case OP_f32_max:
  case OP_f32_copysign:
  case OP_f64_abs:
  case OP_f64_neg:
  case OP_f64_ceil:
  case OP_f64_floor:
  case OP_f64_trunc:
  case OP_f64_nearest:
  case OP_f64_sqrt:
  case OP_f64_add:
  case OP_f64_sub:
  case OP_f64_mul:
  case OP_f64_div:
  case OP_f64_min:
  case OP_f64_max:
  case OP_f64_copysign:
  case OP_i32_wrap_i64:
  case OP_i32_trunc_s_f32:
  case OP_i32_trunc_u_f32:
  case OP_i32_trunc_s_f64:
  case OP_i32_trunc_u_f64:
  case OP_i64_extend_s_i32:
  case OP_i64_extend_u_i32:
  case OP_i64_trunc_s_f32:
  case OP_i64_trunc_u_f32:
  case OP_i64_trunc_s_f64:
  case OP_i64_trunc_u_f64:
  case OP_f32_convert_s_i32:
  case OP_f32_convert_u_i32:
  case OP_f32_convert_s_i64:
  case OP_f32_convert_u_i64:
  case OP_f32_demote_f64:
  case OP_f64_convert_s_i32:
  case OP_f64_convert_u_i32:
  case OP_f64_convert_s_i64:
  case OP_f64_convert_u_i64:
  case OP_f64_promote_f32:
  case OP_i32_reinterpret_f32:
  case OP_i64_reinterpret_f64:
  case OP_f32_reinterpret_i32:
  case OP_f64_reinterpret_i64:
    break;
  default:
    err = ERR_FATAL_UNKNOWN_INSTRUCTION;
  }

  return err;
}

ERROR_CODE ParseTableInit(Stream& s, TableInit& init, Module& m)
{
  ERROR_CODE err = ParseVarUInt32(s, init.index);

  if(err >= 0)
    err = ParseInitializer(s, init.offset);

  if(err >= 0)
  {
    TableDesc* desc = ModuleTable(m, init.index);
    if(!desc)
      err = ERR_INVALID_TABLE_INDEX;
    else if(desc->element_type == TE_anyfunc)
      err = Parse<varuint32>::template Array<&ParseVarUInt32>(s, init.elems, init.n_elems);
    else
      err = ERR_FATAL_BAD_ELEMENT_TYPE;
  }

  return err;
}

typedef struct LocalEntry
{
  varuint32 count;
  varuint7 type;
};

ERROR_CODE ParseLocalEntry(Stream& s, LocalEntry& entry)
{
  ERROR_CODE err = ParseVarUInt32(s, entry.count);

  if(err >= 0)
    err = ParseVarUInt7(s, entry.type);

  return err;
}

ERROR_CODE ParseFunctionBody(Stream& s, FunctionBody& f)
{
  ERROR_CODE err = ParseVarUInt32(s, f.body_size);
  size_t end = s.pos + f.body_size; // body_size is the size of both local_entries and body in bytes.

  if(err >= 0) // Parse local entries into a temporary array, then expand them into a usable local type array.
  {
    LocalEntry* locals;
    varuint32 n_locals;
    err = Parse<LocalEntry>::template Array<&ParseLocalEntry>(s, locals, n_locals);
    if(err < 0)
      return err;

    f.n_locals = 0;
    for(varuint32 i = 0; i < n_locals; ++i)
      f.n_locals += locals[i].count;

    f.locals = tmalloc<varuint7>(f.n_locals);
    f.n_locals = 0;
    for(varuint32 i = 0; i < n_locals; ++i)
      for(varuint32 j = 0; j < locals[i].count; ++j)
        f.locals[f.n_locals++] = locals[i].type;
  }

  f.body = 0;
  if(err >= 0 && f.body_size)
  {
    f.body = tmalloc<Instruction>(f.body_size); // Overallocate maximum number of possible instructions we might have
    if(!f.body)
      return ERR_FATAL_OUT_OF_MEMORY;

    for(f.n_body = 0; s.pos < end && err >= 0; ++f.n_body)
      err = ParseInstruction(s, f.body[f.n_body]);
  }
  f.local_names = 0;
  f.param_names = 0;
  f.debug_name.bytes = 0;

  return err;
}

ERROR_CODE ParseDataInit(Stream& s, DataInit& data)
{
  ERROR_CODE err = ParseVarUInt32(s, data.index);

  if(err >= 0)
    err = ParseInitializer(s, data.offset);

  if(err >= 0)
    err = ParseByteArray(s, data.data, false);

  return err;
}

ERROR_CODE ParseNameSectionLocal(Stream& s, size_t num, const char**& target)
{
  ERROR_CODE err;
  auto index = ReadVarUInt32(s, err);
  if(err < 0) return err;
  if(index >= num)
    return ERR_INVALID_LOCAL_INDEX;

  ByteArray buf;
  err = ParseByteArray(s, buf, true);
  if(err >= 0 && buf.bytes != nullptr)
  {
    if(!target) // Only bother allocating the debug name if there's actually a name to worry about
      target = (const char**)calloc(num, sizeof(const char*));
    target[index] = (const char*)buf.bytes;
  }
  return err;
}

ERROR_CODE ParseNameSection(Stream& s, size_t end, Module& m)
{
  ERROR_CODE err = ERR_SUCCESS;

  while(s.pos < end && err >= ERR_SUCCESS)
  {
    auto type = ReadVarUInt7(s, err);
    if(err < 0) return err;
    auto len = ReadVarUInt32(s, err);
    if(err < 0) return err;

    switch(type)
    {
    case 0: // module
      err = ParseByteArray(s, m.name, true);
      break;
    case 1: // functions
    {
      varuint32 count = ReadVarUInt32(s, err);

      for(varuint32 i = 0; i < count && err >= 0; ++i)
      {
        auto index = ReadVarUInt32(s, err);
        if(err < 0) return err;

        if(index < m.importsection.functions)
        {
          err = ParseByteArray(s, m.importsection.imports[index].func_desc.debug_name, true);
          continue;
        }
        index -= m.importsection.functions;
        if(index >= m.code.n_funcbody)
          return ERR_INVALID_FUNCTION_INDEX;

        err = ParseByteArray(s, m.code.funcbody[index].debug_name, true);
      }
    }
      break;
    case 2: // locals
    {
      varuint32 count = ReadVarUInt32(s, err);

      for(varuint32 i = 0; i < count && err >= 0; ++i)
      {
        auto fn = ReadVarUInt32(s, err);
        if(err < 0) return err;

        if(fn < m.importsection.functions)
        {
          varuint32 num = ReadVarUInt32(s, err);
          auto sig = m.importsection.imports[fn].func_desc.sig_index;
          if(sig >= m.type.n_functions)
            return ERR_INVALID_TYPE_INDEX;

          for(varuint32 j = 0; j < num && err >= 0; ++j)
            ParseNameSectionLocal(s, m.type.functions[sig].n_params, m.importsection.imports[fn].func_desc.param_names);

          continue;
        }

        fn -= m.importsection.functions;
        if(fn >= m.code.n_funcbody)
          return ERR_INVALID_FUNCTION_INDEX;

        varuint32 num = ReadVarUInt32(s, err);

        for(varuint32 j = 0; j < num && err >= 0; ++j)
          ParseNameSectionLocal(s, m.code.funcbody[fn].n_locals, m.code.funcbody[fn].local_names);
      }
    }
      break;
    default:
      s.pos += len;
    }
  }

  return err;
}

ERROR_CODE ParseModule(Stream& s, Module& m, ByteArray name)
{
  memset(&m, 0, sizeof(Module));
  if(!name.bytes || !name.n_bytes || !ValidateIdentifier(name))
    return ERR_PARSE_INVALID_NAME;
  m.name.bytes = tmalloc<uint8_t>(name.n_bytes + 1);
  if(!m.name.bytes)
    return ERR_FATAL_OUT_OF_MEMORY;
  m.name.n_bytes = name.n_bytes;
  memcpy(m.name.bytes, name.bytes, name.n_bytes);
  m.name.bytes[m.name.n_bytes] = 0;

  ERROR_CODE err = ERR_SUCCESS;
  m.magic_cookie = ReadUInt32(s, err);

  if(err >= 0)
    m.version = ReadUInt32(s, err);

  if(err < 0)
    return err;
  if(m.magic_cookie != MAGIC_COOKIE)
    return ERR_PARSE_INVALID_MAGIC_COOKIE;
  if(m.version != MAGIC_VERSION)
    return ERR_PARSE_INVALID_VERSION;

  size_t begin = s.pos;
  m.n_custom = 0; // Count the custom sections so we can preallocate them
  while(!s.End())
  {
    varuint7 op = ReadVarUInt7(s, err);
    if(err < 0)
      return err;

    if(op > SECTION_DATA) // require valid opcode to continue
      break;
    if(op == SECTION_CUSTOM)
      ++m.n_custom;
    s.pos += ReadVarUInt32(s, err);

    if(err < 0)
      return err;
  }

  assert(s.pos == s.size); // If this isn't true something went terribly wrong
  if(s.pos != s.size)
    return ERR_PARSE_INVALID_FILE_LENGTH;

  m.exports = kh_init_exports();
  s.pos = begin;
  size_t curcustom = 0;

  if(m.n_custom > 0)
  {
    m.custom = tmalloc<CustomSection>(m.n_custom);
    if(!m.custom)
      return ERR_FATAL_OUT_OF_MEMORY;
  }

  while(err >= 0 && !s.End())
  {
    varuint7 opcode = ReadVarUInt7(s, err);
    if(err < 0)
      break;
    varuint32 payload = ReadVarUInt32(s, err);
    if(err < 0)
      break;
    if(opcode != SECTION_CUSTOM) // Section order only applies to known sections
    {
      if(!ValidateSectionOrder(m.knownsections, opcode))
        return ERR_FATAL_INVALID_SECTION_ORDER; // This has to be a fatal error because some sections rely on others being loaded
      m.knownsections |= (1 << opcode);
    }

    switch(opcode)
    {
    case SECTION_TYPE:
      err = Parse<FunctionSig>::template Array<&ParseFuncSig>(s, m.type.functions, m.type.n_functions);
      break;
    case SECTION_IMPORT:
    {
      err = Parse<Import>::template Array<&ParseImport>(s, m.importsection.imports, m.importsection.n_import);
      std::stable_sort(m.importsection.imports, m.importsection.imports + m.importsection.n_import, [](const Import& a, const Import& b) -> bool { return a.kind < b.kind; });

      varuint32 num = m.importsection.n_import;
      m.importsection.globals = 0;
      for(varuint32 i = 0; i < num; ++i)
      {
        switch(m.importsection.imports[i].kind)
        {
        case KIND_FUNCTION:
          ++m.importsection.functions;
        case KIND_TABLE:
          ++m.importsection.tables;
        case KIND_MEMORY:
          ++m.importsection.memory;
        case KIND_GLOBAL:
          ++m.importsection.globals;
          break;
        default:
          return ERR_FATAL_UNKNOWN_KIND;
        }
      }

      if(m.importsection.n_import != num) // n_import is the same as globals, check to make sure we derived it properly
        return ERR_FATAL_INVALID_MODULE;
    }
      break;
    case SECTION_FUNCTION:
      err = Parse<varuint32>::template Array<&ParseVarUInt32>(s, m.function.funcdecl, m.function.n_funcdecl);
      break;
    case SECTION_TABLE:
      err = Parse<TableDesc>::template Array<&ParseTableDesc>(s, m.table.tables, m.table.n_tables);
      break;
    case SECTION_MEMORY:
      err = Parse<MemoryDesc>::template Array<&ParseMemoryDesc>(s, m.memory.memory, m.memory.n_memory);
      break;
    case SECTION_GLOBAL:
      err = Parse<GlobalDecl>::template Array<&ParseGlobalDecl>(s, m.global.globals, m.global.n_globals);
      break;
    case SECTION_EXPORT:
      err = Parse<Export>::template Array<&ParseExport>(s, m.exportsection.exports, m.exportsection.n_exports);
      break;
    case SECTION_START:
      m.start = ReadVarUInt32(s, err);
      break;
    case SECTION_ELEMENT:
      err = Parse<TableInit, Module&>::template Array<&ParseTableInit>(s, m.element.elements, m.element.n_elements, m);
      break;
    case SECTION_CODE:
      err = Parse<FunctionBody>::template Array<&ParseFunctionBody>(s, m.code.funcbody, m.code.n_funcbody);
      break;
    case SECTION_DATA:
      err = Parse<DataInit>::template Array<&ParseDataInit>(s, m.data.data, m.data.n_data);
      break;
    case SECTION_CUSTOM:
      assert(curcustom < m.n_custom);
      m.custom[curcustom].payload = payload;
      m.custom[curcustom].data = s.data + s.pos;
      err = ParseIdentifier(s, m.custom[curcustom].name);
      if(err == ERR_SUCCESS && !strcmp((const char*)m.custom[curcustom].name.bytes, "name"))
        ParseNameSection(s, s.pos + payload - m.custom[curcustom].name.n_bytes, m);
      else
        s.pos += payload - m.custom[curcustom].name.n_bytes; // Skip over the custom payload, minus the name
      break;
    default:
      return ERR_FATAL_UNKNOWN_SECTION;
    }
  }

  if(err == ERR_SUCCESS)
    err = ParseExportFixup(m);

  return err;
}

ERROR_CODE ParseExportFixup(Module& m)
{
  for(varuint32 i = 0; i < m.exportsection.n_exports; ++i)
  {
    int r = 0;
    khint_t iter = kh_put_exports(m.exports, (const char*)m.exportsection.exports[i].name.bytes, &r);
    if(r < 0)
      return ERR_FATAL_BAD_HASH;
    if(!r)
      return ERR_FATAL_DUPLICATE_EXPORT;
    kh_value(m.exports, iter) = i;
  }

  return ERR_SUCCESS;
}
