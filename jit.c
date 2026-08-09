#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <janet.h>

typedef Janet (*JitFn)(int32_t argc, Janet *argv);

size_t call_argc = 0;
Janet *call_argv = NULL;


// jit helpers called form emitted assembly
uint64_t jit_typecheck(Janet val, uint32_t types) {
  if (!janet_checktypes(val, types)) {
    janet_panicf("expected %T, got %v", types, val);
  }
}

uint64_t jit_nil(Janet val) {
  if (janet_checktypes(val, JANET_NIL)) {
    return 1;
  } else {
    return 0;
  }
}


uint64_t jit_in(Janet collection, Janet key) {
  // TODO: this one can maybe get a assembly fast path
  return janet_u64(janet_in(collection, key));
}

uint64_t jit_call(Janet callee) {
  uint64_t result;
  if (janet_checktype(callee, JANET_FUNCTION)) {
    JanetFunction *func = janet_unwrap_function(callee);
    result = janet_u64(janet_call(func, call_argc, call_argv));
  } else if (janet_checktype(callee, JANET_CFUNCTION)) {
    JanetCFunction func = janet_unwrap_cfunction(callee);
    result = janet_u64(func(call_argc, call_argv));
  } else if (janet_checktype(callee, JANET_ABSTRACT)) {
    JanetAbstract abstract = janet_unwrap_abstract(callee);
    const JanetAbstractType *at = janet_abstract_type(abstract);
    if (at->call != NULL) {
      result = janet_u64(at->call(abstract, call_argc, call_argv));
    } else {
      janet_panic("attempted to call uncallable abstract type");
    }
  } else if (janet_checktype(callee, JANET_KEYWORD)) {
    if (call_argc == 0) {
      janet_panic("keyword argument on nil value");
    }

    Janet kwcallee = janet_get(call_argv[0], callee);
    if (janet_checktype(kwcallee, JANET_FUNCTION)) {
      JanetFunction *func = janet_unwrap_function(kwcallee);
      result = janet_u64(janet_call(func, call_argc, call_argv));
    } else {
      janet_panicf("keyword function %p, %p is not callable", callee, kwcallee);
    }
  }
  call_argc = 0;
  return result;
}

uint64_t jit_make_array() {
  Janet a = janet_wrap_array(janet_array_n(call_argv, call_argc));
  call_argc = 0;
  return janet_u64(a);
}

uint64_t jit_make_tuple() {
  JanetTuple t = janet_tuple_n(call_argv, call_argc);
  Janet tup = janet_wrap_tuple(t);
  call_argc = 0;
  return janet_u64(tup);
}

uint64_t jit_make_bracket_tuple() {
  JanetTuple t = janet_tuple_n(call_argv, call_argc);
  janet_tuple_flag(t) |= JANET_TUPLE_FLAG_BRACKETCTOR;
  Janet tup = janet_wrap_tuple(t);
  call_argc = 0;
  return janet_u64(tup);
}

uint64_t jit_make_buffer() {
  JanetBuffer *b = janet_buffer(call_argc * 10);
  for (int i = 0; i < call_argc; i++) {
    janet_to_string_b(b, call_argv[i]);
  }
  call_argc = 0;
  return janet_u64(janet_wrap_buffer(b));
}

uint64_t jit_make_string() {
  JanetBuffer *b = janet_buffer(call_argc * 10);
  for (int i = 0; i < call_argc; i++) {
    janet_to_string_b(b, call_argv[i]);
  }
  call_argc = 0;
  // TODO: this leaves a garbage buffer we can potentially skip
  return janet_u64(janet_stringv(b->data, b->count));
}

uint64_t jit_make_table() {
  if (call_argc & 1) {
    janet_panicf("expected even number of arguments to table constructor, got %d", call_argc);
  }
  JanetTable *tab = janet_table(call_argc / 2);
  for (int i = 0; i < call_argc; i += 2) {
    janet_table_put(tab, call_argv[i], call_argv[i + 1]);
  }
  call_argc = 0;
  return janet_u64(janet_wrap_table(tab));
}

uint64_t jit_make_struct() {
  if (call_argc & 1) {
    janet_panicf("expected even number of arguments to struct constructor, got %d", call_argc);
  }
  JanetKV *st = janet_struct_begin(call_argc / 2);
  for (int i = 0; i < call_argc; i += 2) {
    janet_struct_put(st, call_argv[i], call_argv[i + 1]);
  }
  call_argc = 0;
  return janet_u64(janet_wrap_struct(janet_struct_end(st)));
}


void jit_push(Janet value) {
  if (call_argv == NULL) {
    call_argv = malloc(8 * sizeof(Janet));
  }
  call_argv[call_argc++] = value;
}

void jit_push_2(Janet value1, Janet value2) {
  if (call_argv == NULL) {
    call_argv = malloc(8 * sizeof(Janet));
  }
  call_argv[call_argc++] = value1;
  call_argv[call_argc++] = value2;
}

void jit_push_3(Janet value1, Janet value2, Janet value3) {
  if (call_argv == NULL) {
    call_argv = malloc(8 * sizeof(Janet));
  }
  call_argv[call_argc++] = value1;
  call_argv[call_argc++] = value2;
  call_argv[call_argc++] = value3;
}

typedef struct {
  void *code;
  size_t code_size;
  JanetFunction *fallback;
} JittedFunction;

typedef struct {
  uint8_t *data;
  size_t count;
  size_t capacity;
  int *jump_targets;
  int *jump_locations;
  int jump_index;
} CodeBuffer;

static int jitted_function_gc(void *p, size_t size) {
  JittedFunction *jitted = p;
  (void)size;
  if (jitted->code != NULL) {
    munmap(jitted->code, jitted->code_size);
  }
  return 0;
}

static int jitted_function_gcmark(void *p, size_t size) {
  JittedFunction *jitted = p;
  (void)size;
  janet_mark(janet_wrap_function(jitted->fallback));
  return 0;
}

static void emit_byte(CodeBuffer *code, uint8_t byte) {
  if (code->count >= code->capacity) {
    code->capacity *= 2;
    code->data = realloc(code->data, code->capacity);
  }
  code->data[code->count++] = byte;
}

static void emit_u32(CodeBuffer *code, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    emit_byte(code, (uint8_t)(value >> shift));
  }
}

static void emit_u64(CodeBuffer *code, uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    emit_byte(code, (uint8_t)(value >> shift));
  }
}

static void emit_stack_to_gpr(CodeBuffer *code, uint32_t dest, uint32_t source) {
  // xmm(dest), rsi + source
  emit_byte(code, 0xF2);
  emit_byte(code, 0x48 | ((dest & 8) ? 0x04 : 0));
  emit_byte(code, 0x0F);
  emit_byte(code, 0x2C);
  emit_byte(code, 0x84 + ((dest & 7) << 3)); // store here
  emit_byte(code, 0x24);
  emit_u32(code, source * sizeof(Janet)); // from here
}


static void emit_stack_to_xmm(CodeBuffer *code, uint32_t dest, uint32_t source) {
  // xmm(dest), rsi + source
  emit_byte(code, 0xF2);
  emit_byte(code, 0x0F);
  emit_byte(code, 0x10);
  emit_byte(code, 0x84 + (dest << 3));
  emit_byte(code, 0x24);
  emit_u32(code, source * sizeof(Janet));
}

static void emit_xmm_to_stack(CodeBuffer *code, uint32_t dest, uint32_t source) {
  // rsi + source, xmm(dest)
  emit_byte(code, 0xF2);
  emit_byte(code, 0x0F);
  emit_byte(code, 0x11);
  emit_byte(code, 0x84 + (source << 3));
  emit_byte(code, 0x24);
  emit_u32(code, dest * sizeof(Janet));
}


static void emit_binary_op(CodeBuffer *code, uint8_t op, uint32_t dest, uint32_t lhs, uint32_t rhs) {
  // op lhs, rhs
  emit_byte(code, 0xF2);
  emit_byte(code, 0x0F);
  emit_byte(code, op); // operation
  emit_byte(code, 0xC0 + (lhs << 3) + rhs);
}

static void compile_bytecode(CodeBuffer *code, JanetFunction *fn, int pc, uint32_t instr, int stack_size) {
  // TODO: these can be #define/macros, but this is fine for now
  Janet *constants = fn->def->constants;
  int opcode = instr & 0xFF;
  int a = (instr >> 8) & 0xFF;
  int b = (instr >> 16) & 0xFF;
  int c = (instr >> 24) & 0xFF;
  int32_t imm = (int32_t)instr >> 16;
  int32_t imm8 = (int32_t)instr >> 24;
  uint32_t d = (uint32_t)instr >> 8;
  uint32_t e = (uint32_t)instr >> 16;

  switch (opcode) {
  case JOP_NOOP:
    break;
  case JOP_ERROR:
    // put the error in RDI (first arg)
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (7 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)janet_panicv);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    break;
  case JOP_TYPECHECK:
    // a = slot e = type(s)
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (7 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    // immediate type number
    emit_byte(code, 0xBE);
    emit_u32(code, c);
    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)jit_typecheck);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    break;
  case JOP_RETURN:
    // mov rax, rsp + offset
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));

    // restore stack
    if (stack_size > 0) {
      // stack adjust
      emit_byte(code, 0x48);
      emit_byte(code, 0x81);
      emit_byte(code, 0xC4);
      emit_u32(code, stack_size);
    }

    emit_byte(code, 0xC3); // ret
    break;
  case JOP_RETURN_NIL:
    // mov rax, imm(nil)
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_nil()));

    // restore stack
    if (stack_size > 0) {
      // stack adjust
      emit_byte(code, 0x48);
      emit_byte(code, 0x81);
      emit_byte(code, 0xC4);
      emit_u32(code, stack_size);
    }
    emit_byte(code, 0xC3); // ret
    break;
  case JOP_ADD_IMMEDIATE:
    // lhs -> xmm0
    emit_stack_to_xmm(code, 0, b);
    // imm -> rax -> xmm1
    // imm -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_integer(imm8)));
    // rax -> imm1
    emit_byte(code, 0x66);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x6E);
    emit_byte(code, 0xC0 + (1 << 3));
    // +
    emit_binary_op(code, 0x58, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_ADD:
    // lhs -> xmm0
    emit_stack_to_xmm(code, 0, b);
    // rhs -> xmm1
    emit_stack_to_xmm(code, 1, c);
    emit_binary_op(code, 0x58, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_SUBTRACT_IMMEDIATE:
    // lhs -> xmm0
    emit_stack_to_xmm(code, 0, b);
    // imm -> rax -> xmm1
    // imm -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_integer(imm8)));
    // rax -> imm1
    emit_byte(code, 0x66);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x6E);
    emit_byte(code, 0xC0 + (1 << 3));
    // +
    emit_binary_op(code, 0x5C, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_SUBTRACT:
    // lhs -> xmm0
    emit_stack_to_xmm(code, 0, b);
    // rhs -> xmm1
    emit_stack_to_xmm(code, 1, c);
    emit_binary_op(code, 0x5C, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_MULTIPLY_IMMEDIATE:
    // lhs -> xmm0
    emit_stack_to_xmm(code, 0, b);
    // imm -> rax -> xmm1
    // imm -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_integer(imm8)));
    // rax -> imm1
    emit_byte(code, 0x66);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x6E);
    emit_byte(code, 0xC0 + (1 << 3));
    // +
    emit_binary_op(code, 0x59, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_MULTIPLY:
    // lhs -> xmm0
    emit_stack_to_xmm(code, 0, b);
    // rhs -> xxm1
    emit_stack_to_xmm(code, 1, c);
    emit_binary_op(code, 0x59, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_DIVIDE_IMMEDIATE:
    // lhs -> xmm0
    emit_stack_to_xmm(code, 0, b);
    // imm -> rax -> xmm1
    // imm -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_integer(imm8)));
    // rax -> imm1
    emit_byte(code, 0x66);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x6E);
    emit_byte(code, 0xC0 + (1 << 3));
    // +
    emit_binary_op(code, 0x5E, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_DIVIDE:
    // lhs -> xxm0
    emit_stack_to_xmm(code, 0, b);
    // rhs -> xxm1
    emit_stack_to_xmm(code, 1, c);
    emit_binary_op(code, 0x5E, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_DIVIDE_FLOOR:
    // lhs -> xxm0
    emit_stack_to_xmm(code, 0, b);
    // rhs -> xxm1
    emit_stack_to_xmm(code, 1, c);
    emit_binary_op(code, 0x5E, a, 0, 1);
    // floor(xmm0)
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x3A);
    emit_byte(code, 0x0B);
    emit_byte(code, 0xC0 + (0 << 3) + 0);
    emit_byte(code, 0x01); // <- round instead of trunc
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_MODULO:
    // a - truncate(a / b) * b
    // a -> xmm0
    emit_stack_to_xmm(code, 0, b);
    // a -> xmm1
    emit_stack_to_xmm(code, 1, b);
    // b -> xmm2
    emit_stack_to_xmm(code, 2, c);
    // xmm1 / xmm2
    emit_binary_op(code, 0x5E, a, 1, 2);
    // floor(xmm1)
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x3A);
    emit_byte(code, 0x0B);
    emit_byte(code, 0xC0 + (1 << 3) + 1);
    emit_byte(code, 0x01); // <- round instead of trunc
    // xmm1 * xmm2
    emit_binary_op(code, 0x59, a, 1, 2);
    // xmm0 - xmm1
    emit_binary_op(code, 0x5C, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_REMAINDER:
    // a - truncate(a / b) * b
    // a -> xmm0
    emit_stack_to_xmm(code, 0, b);
    // a -> xmm1
    emit_stack_to_xmm(code, 1, b);
    // b -> xmm2
    emit_stack_to_xmm(code, 2, c);
    // xmm1 / xmm2
    emit_binary_op(code, 0x5E, a, 1, 2);
    // trunc(xmm1)
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x3A);
    emit_byte(code, 0x0B);
    emit_byte(code, 0xC0 + (1 << 3) + 1);
    emit_byte(code, 0x03); // <- trunc instead of round
    // xmm1 * xmm2
    emit_binary_op(code, 0x59, a, 1, 2);
    // xmm0 - xmm1
    emit_binary_op(code, 0x5C, a, 0, 1);
    // lhs -> rsp + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_BAND:
    // lhs -> RAX
    emit_stack_to_gpr(code, 0, b);
    // rhs -> RCX
    emit_stack_to_gpr(code, 1, c);
    // (band rax rcx)
    emit_byte(code, 0x48);
    emit_byte(code, 0x21); // and
    emit_byte(code, 0xC8);
    // rax -> xxm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_BOR:
    // lhs -> RAX
    emit_stack_to_gpr(code, 0, b);
    // rhs -> RCX
    emit_stack_to_gpr(code, 1, c);
    // (band rax rcx)
    emit_byte(code, 0x48);
    emit_byte(code, 0x09); // or
    emit_byte(code, 0xC8);
    // rax -> xxm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    // xmm0 -> rsi + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_BXOR:
    // lhs -> RAX
    emit_stack_to_gpr(code, 0, b);
    // rhs -> RCX
    emit_stack_to_gpr(code, 1, c);
    // (bxor rax rcx)
    emit_byte(code, 0x48);
    emit_byte(code, 0x31); // xor
    emit_byte(code, 0xC8);
    // rax -> xxm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_BNOT:
    // e = value
    // value -> RAX
    emit_stack_to_gpr(code, 0, e);
    emit_byte(code, 0x48);
    emit_byte(code, 0xF7);
    emit_byte(code, 0xD0); // NOT

    // rax -> xxm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    // xmm0 -> rsi + offset
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_SHIFT_LEFT:
    emit_stack_to_gpr(code, 0, b);
    emit_stack_to_gpr(code, 1, c);
    // blshift
    emit_byte(code, 0xD3);
    emit_byte(code, 0xE0);
    // rax -> xmm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    // xmm0 -> stack
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_SHIFT_LEFT_IMMEDIATE:
    emit_stack_to_gpr(code, 0, b);
    // shift imm8
    emit_byte(code, 0xC1);
    emit_byte(code, 0xE0);
    emit_byte(code, c);
    // rax -> xmm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    // xmm0 -> stack
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_SHIFT_RIGHT:
    emit_stack_to_gpr(code, 0, b);
    emit_stack_to_gpr(code, 1, c);
    // brshift
    emit_byte(code, 0xD3);
    emit_byte(code, 0xF8);
    // rax -> xmm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    // xmm0 -> stack
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_SHIFT_RIGHT_IMMEDIATE:
    emit_stack_to_gpr(code, 0, b);
    // shift
    emit_byte(code, 0xC1);
    emit_byte(code, 0xF8);
    emit_byte(code, c);
    // rax -> xmm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    // xmm0 -> stack
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_SHIFT_RIGHT_UNSIGNED:
    emit_stack_to_gpr(code, 0, b);
    emit_stack_to_gpr(code, 1, c);
    // brshift
    emit_byte(code, 0xD3);
    emit_byte(code, 0xF8);
    // rax -> xmm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    // xmm0 -> stack
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_SHIFT_RIGHT_UNSIGNED_IMMEDIATE:
    emit_stack_to_gpr(code, 0, b);
    // shift
    emit_byte(code, 0xC1);
    emit_byte(code, 0xF8);
    emit_byte(code, c);
    // rax -> xmm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    // xmm0 -> stack
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_MOVE_FAR:
    emit_stack_to_xmm(code, 0, a);
    emit_xmm_to_stack(code, e, 0);
    break;
  case JOP_MOVE_NEAR:
    emit_stack_to_xmm(code, 0, e);
    emit_xmm_to_stack(code, a, 0);
    break;
  case JOP_JUMP:
    emit_byte(code, 0xE9);
    // location to patch in the 32bit destination
    code->jump_targets[code->jump_index] = pc + ((int32_t)instr >> 8);
    code->jump_locations[code->jump_index] = code->count;
    code->jump_index++;
    emit_u32(code, 0);
    break;
  case JOP_JUMP_IF:
    // a -> arg 1
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (7 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    // test if truthy in interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)janet_truthy);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // compare
    emit_byte(code, 0x85);
    emit_byte(code, 0xC0);
    // conditional jump
    emit_byte(code, 0x0F);
    emit_byte(code, 0x85); // JNZ
    // jump location to patch later
    code->jump_targets[code->jump_index] = pc + ((int32_t)instr >> 16);
    code->jump_locations[code->jump_index] = code->count;
    code->jump_index++;
    emit_u32(code, 0);
    break;
  case JOP_JUMP_IF_NOT:
    // a -> arg 1
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (7 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    // test if truthy in interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)janet_truthy);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // compare
    emit_byte(code, 0x85);
    emit_byte(code, 0xC0);
    // conditional jump
    emit_byte(code, 0x0F);
    emit_byte(code, 0x84); // JZ
    // jump location to patch later
    code->jump_targets[code->jump_index] = pc + ((int32_t)instr >> 16);
    code->jump_locations[code->jump_index] = code->count;
    code->jump_index++;
    emit_u32(code, 0);
    break;
  case JOP_JUMP_IF_NIL:
    // a -> arg 1
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (7 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    // test if truthy in interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)jit_nil);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // compare
    emit_byte(code, 0x85);
    emit_byte(code, 0xC0);
    // conditional jump
    emit_byte(code, 0x0F);
    emit_byte(code, 0x85); // JNZ
    // jump location to patch later
    code->jump_targets[code->jump_index] = pc + ((int32_t)instr >> 16);
    code->jump_locations[code->jump_index] = code->count;
    code->jump_index++;
    emit_u32(code, 0);
    break;
  case JOP_JUMP_IF_NOT_NIL:
    // a -> arg 1
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (7 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    // test if truthy in interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)jit_nil);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // compare
    emit_byte(code, 0x85);
    emit_byte(code, 0xC0);
    // conditional jump
    emit_byte(code, 0x0F);
    emit_byte(code, 0x84); // JZ
    // jump location to patch later
    code->jump_targets[code->jump_index] = pc + ((int32_t)instr >> 16);
    code->jump_locations[code->jump_index] = code->count;
    code->jump_index++;
    emit_u32(code, 0);
    break;
  case JOP_GREATER_THAN:
    emit_stack_to_xmm(code, 0, b);
    emit_stack_to_xmm(code, 1, c);
    // TODO: use this in JOP_COMPARE
    // ucomisd left, right sets ZF when equal and PF when either value is NaN
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2E);
    emit_byte(code, 0xC0 + (0 << 3) + 1);
    // set AL based on result of comparison
    // AL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x97); // SETA
    emit_byte(code, 0xC0); // AL
    // DL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x9B); // SETNP - make nan not equal
    emit_byte(code, 0xC2); // DL
    // (and al dl)
    emit_byte(code, 0x20);
    emit_byte(code, 0xD0);
    // store al
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xD0); // MOVZX EDX, AL.
    // false -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_false()));
     // OR RAX, RDX
    emit_byte(code, 0x48);
    emit_byte(code, 0x09);
    emit_byte(code, 0xD0);
    // RAX -> stack + offset
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    break;
  case JOP_GREATER_THAN_IMMEDIATE:
    emit_stack_to_xmm(code, 0, b);
    // imm -> rax -> xmm1
    // imm -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_integer(imm8)));
    // rax -> imm1
    emit_byte(code, 0x66);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x6E);
    emit_byte(code, 0xC0 + (1 << 3));
    // ucomisd left, right sets ZF when equal and PF when either value is NaN
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2E);
    emit_byte(code, 0xC0 + (0 << 3) + 1);
    // set AL based on result of comparison
    // AL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x97); // SETA
    emit_byte(code, 0xC0); // AL
    // DL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x9B); // SETNP - make nan not equal
    emit_byte(code, 0xC2); // DL
    // (and al dl)
    emit_byte(code, 0x20);
    emit_byte(code, 0xD0);
    // store al
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xD0); // MOVZX EDX, AL.
    // false -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_false()));
     // OR RAX, RDX
    emit_byte(code, 0x48);
    emit_byte(code, 0x09);
    emit_byte(code, 0xD0);
    // RAX -> stack + offset
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    break;
  case JOP_GREATER_THAN_EQUAL:
    emit_stack_to_xmm(code, 0, b);
    emit_stack_to_xmm(code, 1, c);
    // TODO: use this in JOP_COMPARE
    // ucomisd left, right sets ZF when equal and PF when either value is NaN
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2E);
    emit_byte(code, 0xC0 + (0 << 3) + 1);
    // set AL based on result of comparison
    // AL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x93); // SETAE
    emit_byte(code, 0xC0); // AL
    // DL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x9B); // SETNP - make nan not equal
    emit_byte(code, 0xC2); // DL
    // (and al dl)
    emit_byte(code, 0x20);
    emit_byte(code, 0xD0);
    // store al
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xD0); // MOVZX EDX, AL.
    // false -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_false()));
     // OR RAX, RDX
    emit_byte(code, 0x48);
    emit_byte(code, 0x09);
    emit_byte(code, 0xD0);
    // RAX -> stack + offset
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    break;
  case JOP_LESS_THAN:
    emit_stack_to_xmm(code, 0, b);
    emit_stack_to_xmm(code, 1, c);
    // TODO: use this in JOP_COMPARE
    // ucomisd left, right sets ZF when equal and PF when either value is NaN
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2E);
    emit_byte(code, 0xC0 + (0 << 3) + 1);
    // set AL based on result of comparison
    // AL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x92); // SETB
    emit_byte(code, 0xC0); // AL
    // DL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x9B); // SETNP - make nan not equal
    emit_byte(code, 0xC2); // DL
    // (and al dl)
    emit_byte(code, 0x20);
    emit_byte(code, 0xD0);
    // store al
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xD0); // MOVZX EDX, AL.
    // false -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_false()));
     // OR RAX, RDX
    emit_byte(code, 0x48);
    emit_byte(code, 0x09);
    emit_byte(code, 0xD0);
    // RAX -> stack + offset
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    break;
  case JOP_LESS_THAN_IMMEDIATE:
    emit_stack_to_xmm(code, 0, b);
    // imm -> rax -> xmm1
    // imm -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_integer(imm8)));
    // rax -> imm1
    emit_byte(code, 0x66);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x6E);
    emit_byte(code, 0xC0 + (1 << 3));
    // ucomisd left, right sets ZF when equal and PF when either value is NaN
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2E);
    emit_byte(code, 0xC0 + (0 << 3) + 1);
    // set AL based on result of comparison
    // AL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x92); // SETB
    emit_byte(code, 0xC0); // AL
    // DL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x9B); // SETNP - make nan not equal
    emit_byte(code, 0xC2); // DL
    // (and al dl)
    emit_byte(code, 0x20);
    emit_byte(code, 0xD0);
    // store al
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xD0); // MOVZX EDX, AL.
    // false -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_false()));
     // OR RAX, RDX
    emit_byte(code, 0x48);
    emit_byte(code, 0x09);
    emit_byte(code, 0xD0);
    // RAX -> stack + offset
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    break;
  case JOP_LESS_THAN_EQUAL:
    emit_stack_to_xmm(code, 0, b);
    emit_stack_to_xmm(code, 1, c);
    // TODO: use this in JOP_COMPARE
    // ucomisd left, right sets ZF when equal and PF when either value is NaN
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2E);
    emit_byte(code, 0xC0 + (0 << 3) + 1);
    // set AL based on result of comparison
    // AL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x96); // SETBE
    emit_byte(code, 0xC0); // AL
    // DL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x9B); // SETNP - make nan not equal
    emit_byte(code, 0xC2); // DL
    // (and al dl)
    emit_byte(code, 0x20);
    emit_byte(code, 0xD0);
    // store al
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xD0); // MOVZX EDX, AL.
    // false -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_false()));
    // OR RAX, RDX
    emit_byte(code, 0x48);
    emit_byte(code, 0x09);
    emit_byte(code, 0xD0);
    // RAX -> stack + offset
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    break;
  case JOP_EQUALS:
    emit_stack_to_xmm(code, 0, b);
    emit_stack_to_xmm(code, 1, c);
    // TODO: use this in JOP_COMPARE
    // ucomisd left, right sets ZF when equal and PF when either value is NaN
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2E);
    emit_byte(code, 0xC0 + (0 << 3) + 1);
    // set AL based on result of comparison
    // AL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x94); // SETE
    emit_byte(code, 0xC0); // AL
    // DL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x9B); // SETNP - make nan not equal
    emit_byte(code, 0xC2); // DL
    // (and al dl)
    emit_byte(code, 0x20);
    emit_byte(code, 0xD0);
    // store al
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xD0); // MOVZX EDX, AL.
    // false -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_false()));
     // OR RAX, RDX
    emit_byte(code, 0x48);
    emit_byte(code, 0x09);
    emit_byte(code, 0xD0);
    // RAX -> stack + offset
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    break;
  case JOP_EQUALS_IMMEDIATE:
    emit_stack_to_xmm(code, 0, b);
    // imm -> rax -> xmm1
    // imm -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_integer(imm8)));
    // rax -> imm1
    emit_byte(code, 0x66);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x6E);
    emit_byte(code, 0xC0 + (1 << 3));
    // ucomisd left, right sets ZF when equal and PF when either value is NaN
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2E);
    emit_byte(code, 0xC0 + (0 << 3) + 1);
    // set AL based on result of comparison
    // AL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x94); // SETE
    emit_byte(code, 0xC0); // AL
    // DL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x9B); // SETNP - make nan not equal
    emit_byte(code, 0xC2); // DL
    // (and al dl)
    emit_byte(code, 0x20);
    emit_byte(code, 0xD0);
    // store al
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xD0); // MOVZX EDX, AL.
    // false -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_false()));
     // OR RAX, RDX
    emit_byte(code, 0x48);
    emit_byte(code, 0x09);
    emit_byte(code, 0xD0);
    // RAX -> stack + offset
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    break;
  case JOP_NOT_EQUALS:
    emit_stack_to_xmm(code, 0, b);
    emit_stack_to_xmm(code, 1, c);
    // TODO: use this in JOP_COMPARE
    // ucomisd left, right sets ZF when equal and PF when either value is NaN
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2E);
    emit_byte(code, 0xC0 + (0 << 3) + 1);
    // set AL based on result of comparison
    // AL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x95); // SETNE
    emit_byte(code, 0xC0); // AL
    // DL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x9B); // SETNP - make nan not equal
    emit_byte(code, 0xC2); // DL
    // (and al dl)
    emit_byte(code, 0x20);
    emit_byte(code, 0xD0);
    // store al
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xD0); // MOVZX EDX, AL.
    // false -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_false()));
     // OR RAX, RDX
    emit_byte(code, 0x48);
    emit_byte(code, 0x09);
    emit_byte(code, 0xD0);
    // RAX -> stack + offset
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    break;
  case JOP_NOT_EQUALS_IMMEDIATE:
    emit_stack_to_xmm(code, 0, b);
    // imm -> rax -> xmm1
    // imm -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_integer(imm8)));
    // rax -> imm1
    emit_byte(code, 0x66);
    emit_byte(code, 0x48);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x6E);
    emit_byte(code, 0xC0 + (1 << 3));
    // ucomisd left, right sets ZF when equal and PF when either value is NaN
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2E);
    emit_byte(code, 0xC0 + (0 << 3) + 1);
    // set AL based on result of comparison
    // AL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x95); // SETNE
    emit_byte(code, 0xC0); // AL
    // DL
    emit_byte(code, 0x0F);
    emit_byte(code, 0x9B); // SETNP - make nan not equal
    emit_byte(code, 0xC2); // DL
    // (and al dl)
    emit_byte(code, 0x20);
    emit_byte(code, 0xD0);
    // store al
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xD0); // MOVZX EDX, AL.
    // false -> rax
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_false()));
    // OR RAX, RDX
    emit_byte(code, 0x48);
    emit_byte(code, 0x09);
    emit_byte(code, 0xD0);
    // RAX -> stack + offset
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    break;
  case JOP_COMPARE:
    emit_stack_to_xmm(code, 0, b);
    emit_stack_to_xmm(code, 1, c);
    // compare
    emit_byte(code, 0x66);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2E);
    emit_byte(code, 0xC0 + (0 << 3) + 1);
    // seta al
    emit_byte(code, 0x0F);
    emit_byte(code, 0x97); // SETA -- above
    emit_byte(code, 0xC0); // AL
    // setb dl
    emit_byte(code, 0x0F);
    emit_byte(code, 0x92); // SETB -- below
    emit_byte(code, 0xC2); // DL
    // setp cl
    emit_byte(code, 0x0F);
    emit_byte(code, 0x9A); // SETP - make nan not equal
    emit_byte(code, 0xC1); // CL
    // movzx, eax, al
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xC0);
    // movzx ecx, cl
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xC9);
    // zero extend eax
    emit_byte(code, 0x0F);
    emit_byte(code, 0xB6);
    emit_byte(code, 0xD2);
    // sub eax, edx
    emit_byte(code, 0x29);
    emit_byte(code, 0xD0);
    // lea eax, rax + rcx*2
    emit_byte(code, 0x8D);
    emit_byte(code, 0x04);
    emit_byte(code, 0x48);
    // eax -> xmm0
    emit_byte(code, 0xF2);
    emit_byte(code, 0x0F);
    emit_byte(code, 0x2A);
    emit_byte(code, 0xC0);
    emit_xmm_to_stack(code, a, 0);

    // TODO use this as the fallback
    /* // TODO: this can also probably have a fast path */
    /* // b = lhs c = rhs */
    /* // lhs 7 (first arg) */
    /* emit_byte(code, 0x48); */
    /* emit_byte(code, 0x8B); */
    /* emit_byte(code, 0x84 + (7 << 3)); */
    /* emit_byte(code, 0x24); */
    /* emit_u32(code, b * sizeof(Janet)); */
    /* // rhs 6 (second arg) */
    /* emit_byte(code, 0x48); */
    /* emit_byte(code, 0x8B); */
    /* emit_byte(code, 0x84 + (6 << 3)); */
    /* emit_byte(code, 0x24); */
    /* emit_u32(code, c * sizeof(Janet)); */
    /* // go back to the interpreter */
    /* emit_byte(code, 0x48); */
    /* emit_byte(code, 0xB8); */
    /* emit_u64(code, (uint64_t)(uintptr_t)janet_compare); */
    /* emit_byte(code, 0xFF); */
    /* emit_byte(code, 0xD0); */
    /* // store return value (as int) */
    /* emit_byte(code, 0xF2); */
    /* emit_byte(code, 0x0F); */
    /* emit_byte(code, 0x2A); */
    /* emit_byte(code, 0xC0); */
    /* emit_xmm_to_stack(code, a, 0); */
    break;
  case JOP_LOAD_NIL:
    // load to rax, immediate
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_nil()));

    // mov rsp + offset, rax
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, d * sizeof(Janet));
    break;
  case JOP_LOAD_TRUE:
    // load to rax, immediate
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_true()));

    // mov rsp + offset, rax
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, d * sizeof(Janet));
    break;
  case JOP_LOAD_FALSE:
    // load to rax, immediate
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_false()));

    // mov rsp + offset, rax
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, d * sizeof(Janet));
    break;
  case JOP_LOAD_INTEGER:
    // load to rax, immediate
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_integer(imm)));

    // mov rsp + offset, rax
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    break;
  case JOP_LOAD_CONSTANT:
    // load to rax, immediate
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(constants[e]));
    // store rax on stack
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    break;
  case JOP_LOAD_UPVALUE:
    janet_panic("janet's upvalue opcode is not supported");
    break;
  case JOP_LOAD_SELF:
    // load to rax, immediate
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, janet_u64(janet_wrap_function(fn)));
    // store rax on stack
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, d * sizeof(Janet));
    break;
  case JOP_SET_UPVALUE:
    janet_panic("janet's set upvalue opcode is not supported");
    break;
  case JOP_CLOSURE:
    janet_panic("janet's closure opcode is not supported");
    break;
  case JOP_PUSH:
    // d = value to push
    // put the error in RDI (first arg)
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (7 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, d * sizeof(Janet));
    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)jit_push);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    break;
  case JOP_PUSH_2:
    // a, e
    // put the error in RDI (first arg)
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (7 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));

    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (6 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, e * sizeof(Janet));

    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)jit_push_2);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    break;
  case JOP_PUSH_3:
    // a, b, c
    // put the error in RDI (first arg)
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (7 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));

    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (6 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, b * sizeof(Janet));

    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (2 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, c * sizeof(Janet));

    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)jit_push_3);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    break;
    /* case JOP_PUSH_ARRAY: */
  case JOP_CALL:
    // a = dest e = callee
    // put the error in RDI (first arg)
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (7 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, e * sizeof(Janet));
    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)jit_call);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // store return value
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet)); // stack location
    break;
  case JOP_TAILCALL:
    // d = callee
    // put the error in RDI (first arg)
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (7 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, d * sizeof(Janet));
    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)jit_call);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // store return value
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet)); // stack location

    // restore stack
    if (stack_size > 0) {
      // stack adjust
      emit_byte(code, 0x48);
      emit_byte(code, 0x81);
      emit_byte(code, 0xC4);
      emit_u32(code, stack_size);
    }

    emit_byte(code, 0xC3); // ret
    break;
  case JOP_RESUME:
    janet_panic("janet's resume opcode is not supported");
    break;
  case JOP_SIGNAL:
    janet_panic("janet's signal opcode is not supported");
    break;
  case JOP_PROPAGATE:
    janet_panic("janet's propagate opcode is not supported");
    break;
  case JOP_IN:
    // b = collection, c = key
    // collection arg 1 (7)
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (7 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, b * sizeof(Janet));
    // key (6)
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (6 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, c * sizeof(Janet));
    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)janet_in);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // store return value
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet)); // stack location
    break;
  case JOP_GET:
    // b = collection, c = key
    // collection arg 1 (7)
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (7 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, b * sizeof(Janet));
    // key (6)
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (6 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, c * sizeof(Janet));
    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)janet_get);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // store return value
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet)); // stack location
    break;
  case JOP_GET_INDEX:
    // b = collection, c = key
    // collection arg 1 (7)
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (7 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, b * sizeof(Janet));
    // key (6)
    // c immidiate (6)
    emit_byte(code, 0xBE);
    emit_u32(code, c);
    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)janet_getindex);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // store return value
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet)); // stack location
    break;
  case JOP_PUT:
    // a = collection, b = key, c = val
    // collection arg 1 (7)
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (7 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));
    // key (6)
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (6 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, b * sizeof(Janet));
    // val (2)
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (2 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, c * sizeof(Janet));
    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)janet_put);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    break;
  case JOP_PUT_INDEX:
    // a = collection, c = key b = value
    // collection arg 1 (7)
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (7 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet));

    // c immidiate (6)
    emit_byte(code, 0xBE);
    emit_u32(code, c);

    // val (2)
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (2 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, b * sizeof(Janet));

    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)janet_putindex);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    break;
  case JOP_LENGTH:
    // e = collection
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (7 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, e * sizeof(Janet));
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)janet_lengthv);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // store return value
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet)); // stack location
    break;
  case JOP_MAKE_ARRAY:
    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)jit_make_array);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // store return value
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet)); // stack location
    break;
  case JOP_MAKE_TUPLE:
    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)jit_make_tuple);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // store return value
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet)); // stack location
    break;
  case JOP_MAKE_BRACKET_TUPLE:
    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)jit_make_bracket_tuple);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // store return value
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet)); // stack location
    break;
  case JOP_MAKE_BUFFER:
    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)jit_make_buffer);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // store return value
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet)); // stack location
    break;
  case JOP_MAKE_STRING:
    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)jit_make_string);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // store return value
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet)); // stack location
    break;
  case JOP_MAKE_STRUCT:
    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)jit_make_struct);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // store return value
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet)); // stack location
    break;
  case JOP_MAKE_TABLE:
    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)jit_make_table);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // store return value
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet)); // stack location
    break;
  case JOP_NEXT:
    // b = collection, c = key
    // collection arg 1 (7)
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (7 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, b * sizeof(Janet));
    // key (6)
    emit_byte(code, 0x48);
    emit_byte(code, 0x8B);
    emit_byte(code, 0x84 + (6 << 3));
    emit_byte(code, 0x24);
    emit_u32(code, c * sizeof(Janet));
    // go back to the interpreter
    emit_byte(code, 0x48);
    emit_byte(code, 0xB8);
    emit_u64(code, (uint64_t)(uintptr_t)janet_next);
    emit_byte(code, 0xFF);
    emit_byte(code, 0xD0);
    // store return value
    emit_byte(code, 0x48);
    emit_byte(code, 0x89);
    emit_byte(code, 0x84);
    emit_byte(code, 0x24);
    emit_u32(code, a * sizeof(Janet)); // stack location
    break;
  case JOP_CANCEL:
    janet_panic("janet's cancel opcode is not supported");
    break;
  default:
    janet_panic("unsupported op");
    return;
  }
}

int jitted_compile(JittedFunction *jitted, JanetFunction *fn) {
  JanetFuncDef *def = fn->def;
  size_t arity = def->arity;
  size_t bc_len = def->bytecode_length;
  int32_t def_slots = def->slotcount;

  CodeBuffer code = {
    malloc(256 * sizeof(uint8_t)),
    0,
    256,
    malloc(256 * sizeof(uint32_t)),
    malloc(256 * sizeof(uint32_t)),
    0
  };

  int def_size = def_slots * sizeof(Janet);
  int stack_size = ((def_size + 8 + 15) & ~15) - 8;

  if (stack_size > 0) {
    // stack adjust
    emit_byte(&code, 0x48);             // REX.W: use 64-bit operands.
    emit_byte(&code, 0x81);             // Group 1 arithmetic on r/m64 with imm32.
    emit_byte(&code, 0xEC);             // 0xEC: sub rsp, imm32; 0xC4: add rsp, imm32.
    emit_u32(&code, stack_size); // Stack-frame size.
  }

  // copy args to stack frame indexes
  for (int i = 0; i < arity; i++) {
    uint32_t offset = i * sizeof(Janet);
    // argv[offset] -> RAX
    emit_byte(&code, 0x48);
    emit_byte(&code, 0x8B); // MOV rax [rsi + offset]
    emit_byte(&code, 0x86);
    emit_u32(&code, offset);

    // RAX -> stack + offset
    emit_byte(&code, 0x48);
    emit_byte(&code, 0x89);
    emit_byte(&code, 0x84);
    emit_byte(&code, 0x24);
    emit_u32(&code, offset);
  }

  int *instruction_byte_locations = malloc(bc_len * sizeof(int));
  // copile bytecode
  for (int i = 0; i < bc_len; i++) {
    instruction_byte_locations[i] = code.count;
    compile_bytecode(&code, fn, i, def->bytecode[i], stack_size);
  }

  // patch jumps
  for (int i = 0; i < code.jump_index; i++) {
    int dest = instruction_byte_locations[code.jump_targets[i]];

    int distance = dest - (code.jump_locations[i] + 4);
    for (int shift = 0; shift < 32; shift += 8) {
      code.data[code.jump_locations[i] + shift / 8] = (distance >> shift);
    }
  }



  void * mapping = mmap(NULL,
			code.count,
			PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS,
			-1,
			0);

  if (mapping == MAP_FAILED) {
    int error = errno;
    free(code.data);
    janet_panicf("could not allocate JIT memory: %s", strerror(error));
  }

  memcpy(mapping, code.data, code.count);

  if (mprotect(mapping, code.count, PROT_READ | PROT_EXEC) != 0) {
    int error = errno;
    munmap(mapping, code.count);
    janet_panicf("could not make JIT memory executable: %s", strerror(error));
  }

  jitted->code = mapping;
  jitted->code_size = code.count;
  free(code.data);
  return 0;
}

static Janet jitted_function_call(void *p, int32_t argc, Janet *argv) {
  // for now, assume we are only doing math and all args are always numbers
  // we also assume that Janet objects are always nonboxed
  JittedFunction *jitted = p;

  if (jitted->code == NULL) {
    uint8_t *code = malloc(256 * sizeof(uint8_t));
    jitted_compile(jitted, jitted->fallback);
  }

  return ((JitFn)jitted->code)(argc, argv);
}

static const JanetAbstractType jitted_function_type = {
  "jittable-function",
  jitted_function_gc,
  jitted_function_gcmark,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  jitted_function_call,
  NULL,
  NULL,
  NULL
};

static Janet jit_jitable(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);

  JittedFunction *jitted =
    janet_abstract(&jitted_function_type, sizeof(JittedFunction));

  jitted->code = NULL;
  jitted->fallback = janet_getfunction(argv, 0);

  return janet_wrap_abstract(jitted);
}

static Janet jit_compiled(int32_t argc, Janet *argv) {
  return janet_wrap_true();
}

static const JanetReg cfuns[] = {
  {"jitable", jit_jitable, "(jit/jitable function)\n\nReturns a Jittable version of a function. Jit compilation happens on first call."},
  {"compiled?", jit_compiled, "(jit/compiled? function)\n\nReturns if the function successfully compiled."},
  {NULL, NULL, NULL}
};

JANET_MODULE_ENTRY(JanetTable *env) {
  janet_cfuns(env, "jit", cfuns);
  janet_register_abstract_type(&jitted_function_type);
}
