#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdbool.h>
#include <inttypes.h>

#include "janet.h"

typedef struct BasicBlock BasicBlock;

typedef enum {
  OPERAND_UNUSED,
  OPERAND_UNDEFINED,
  OPERAND_VIRTUAL_REGISTER,
  OPERAND_BASIC_BLOCK,
  OPERAND_SIGNED_IMM,
  OPERAND_UNSIGNED_IMM,
  OPERAND_JANET_IMM,
  OPERAND_PHI_SOURCE,
} OperandType;

char *operand_names[] = {
  "unused",
  "undefined",
  "vreg",
  "bb",
  "imms",
  "immus",
  "jimm",
  "phi_source",
};

typedef struct {
  size_t bb;
  size_t virtual_register;
  size_t slot;
} PhiSource;


typedef struct {
  bool on_stack;
  size_t reg;
  // This always matches the vreg number, but can be made smaller.
  // It only needs to be as high as the top we can get to with
  // our registers allocated.
  size_t stack_offset;
} PhysicalLocation;

typedef struct {
  OperandType type;
  union {
    size_t bb;
    size_t virtual_register;
    uint64_t immus;
    int64_t imms;
  };
  PhysicalLocation physical_location;
} Operand;

typedef enum {
  HIR_NOOP,
  HIR_ERROR,
  HIR_TYPECHECK,
  HIR_RETURN,
  HIR_RETURN_NIL,
  HIR_ADD,
  HIR_ADD_IMM,
  HIR_SUB,
  HIR_SUB_IMM,
  HIR_MUL,
  HIR_MUL_IMM,
  HIR_DIV,
  HIR_DIV_IMM,
  HIR_DIV_FLOOR,
  HIR_MODULO,
  HIR_REMAINDER,
  HIR_AND,
  HIR_OR,
  HIR_XOR,
  HIR_NOT,
  HIR_LSHIFT,
  HIR_LSHIFT_IMM,
  HIR_RSHIFT,
  HIR_RSHIFT_IMM,
  HIR_RUSHIFT,
  HIR_RUSHIFT_IMM,
  HIR_JUMP,
  HIR_JUMP_IF,
  HIR_JUMP_IF_NOT,
  HIR_JUMP_IF_NIL,
  HIR_JUMP_IF_NOT_NIL,
  HIR_GREATER_THAN,
  HIR_GREATER_THAN_IMM,
  HIR_LESS_THAN,
  HIR_LESS_THAN_IMM,
  HIR_EQUALS,
  HIR_EQUALS_IMM,
  HIR_COMPARE,
  HIR_LOAD,
  HIR_LOAD_ARG,
  HIR_PUSH,
  HIR_PUSH2,
  HIR_PUSH3,
  HIR_CALL,
  HIR_TAIL_CALL,
  HIR_IN,
  HIR_GET,
  HIR_GET_INDEX,
  HIR_PUT,
  HIR_PUT_INDEX,
  HIR_LENGTH,
  HIR_MAKE_ARRAY,
  HIR_MAKE_BUFFER,
  HIR_MAKE_STRING,
  HIR_MAKE_STRUCT,
  HIR_MAKE_TABLE,
  HIR_MAKE_TUPLE,
  HIR_MAKE_BRACKET_TUPLE,
  HIR_GREATER_THAN_EQUAL,
  HIR_LESS_THAN_EQUAL,
  HIR_NEXT,
  HIR_NOT_EQUALS,
  HIR_NOT_EQUALS_IMM,
  HIR_PHI,
  HIR_PHI_PLACEHOLDER,
} InstructionType;

char *instruction_names[] = {
  [HIR_NOOP] = "noop",
  [HIR_ERROR] = "err",
  [HIR_TYPECHECK] = "typecheck",
  [HIR_RETURN] = "ret",
  [HIR_RETURN_NIL] = "ret-nil",
  [HIR_ADD] = "add",
  [HIR_ADD_IMM] = "addi",
  [HIR_SUB] = "sub",
  [HIR_SUB_IMM] = "subi",
  [HIR_MUL] = "mul",
  [HIR_MUL_IMM] = "muli",
  [HIR_DIV] = "div",
  [HIR_DIV_IMM] = "divi",
  [HIR_DIV_FLOOR] = "divf",
  [HIR_MODULO] = "mod",
  [HIR_REMAINDER] = "rem",
  [HIR_AND] = "and",
  [HIR_OR] = "or",
  [HIR_XOR] = "xor",
  [HIR_NOT] = "not",
  [HIR_LSHIFT] = "lshift",
  [HIR_LSHIFT_IMM] = "lshifti",
  [HIR_RSHIFT] = "rshift",
  [HIR_RSHIFT_IMM] = "rshifti",
  [HIR_RUSHIFT] = "rushift",
  [HIR_RUSHIFT_IMM] = "rushifti",
  [HIR_JUMP] = "jump",
  [HIR_JUMP_IF] = "jump-if",
  [HIR_JUMP_IF_NOT] = "jump-if-not",
  [HIR_JUMP_IF_NIL] = "jump-if-nil",
  [HIR_JUMP_IF_NOT_NIL] = "jump-if-not-nil",
  [HIR_GREATER_THAN] = "gt",
  [HIR_GREATER_THAN_IMM] = "gti",
  [HIR_LESS_THAN] = "lt",
  [HIR_LESS_THAN_IMM] = "lti",
  [HIR_EQUALS] = "eq",
  [HIR_EQUALS_IMM] = "eqi",
  [HIR_COMPARE] = "comp",
  [HIR_LOAD] = "load",
  [HIR_LOAD_ARG] = "load-arg",
  [HIR_PUSH] = "push",
  [HIR_PUSH2] = "push2",
  [HIR_PUSH3] = "push3",
  [HIR_CALL] = "call",
  [HIR_TAIL_CALL] = "tcall",
  [HIR_IN] = "in",
  [HIR_GET] = "get",
  [HIR_GET_INDEX] = "geti",
  [HIR_PUT] = "put",
  [HIR_PUT_INDEX] = "puti",
  [HIR_LENGTH] = "length",
  [HIR_MAKE_ARRAY] = "make-array",
  [HIR_MAKE_BUFFER] = "make-buffer",
  [HIR_MAKE_STRING] = "make-string",
  [HIR_MAKE_STRUCT] = "make-struct",
  [HIR_MAKE_TABLE] = "make-table",
  [HIR_MAKE_TUPLE] = "make-tuple",
  [HIR_MAKE_BRACKET_TUPLE] = "make-bracket-tuple",
  [HIR_GREATER_THAN_EQUAL] = "gte",
  [HIR_LESS_THAN_EQUAL] = "lte",
  [HIR_NEXT] = "next",
  [HIR_NOT_EQUALS] = "not-eq",
  [HIR_NOT_EQUALS_IMM] = "not-eqi",
  [HIR_PHI] = "phi",
  [HIR_PHI_PLACEHOLDER] = "impossible",
};

size_t jop_to_hir[] = {
  [JOP_NOOP] = HIR_NOOP,
  [JOP_ERROR] = HIR_ERROR,
  [JOP_TYPECHECK] = HIR_TYPECHECK,
  [JOP_RETURN] = HIR_RETURN,
  [JOP_RETURN_NIL] = HIR_RETURN,
  [JOP_ADD_IMMEDIATE] = HIR_ADD_IMM,
  [JOP_ADD] = HIR_ADD,
  [JOP_SUBTRACT_IMMEDIATE] = HIR_SUB_IMM,
  [JOP_SUBTRACT] = HIR_SUB,
  [JOP_MULTIPLY_IMMEDIATE] = HIR_MUL_IMM,
  [JOP_MULTIPLY] = HIR_MUL,
  [JOP_DIVIDE_IMMEDIATE] = HIR_DIV_IMM,
  [JOP_DIVIDE] = HIR_DIV,
  [JOP_DIVIDE_FLOOR] = HIR_DIV_FLOOR,
  [JOP_MODULO] = HIR_MODULO,
  [JOP_REMAINDER] = HIR_REMAINDER,
  [JOP_BAND] = HIR_AND,
  [JOP_BOR] = HIR_OR,
  [JOP_BXOR] = HIR_XOR,
  [JOP_BNOT] = HIR_NOT,
  [JOP_SHIFT_LEFT] = HIR_LSHIFT,
  [JOP_SHIFT_LEFT_IMMEDIATE] = HIR_LSHIFT_IMM,
  [JOP_SHIFT_RIGHT] = HIR_RSHIFT,
  [JOP_SHIFT_RIGHT_IMMEDIATE] = HIR_RSHIFT_IMM,
  [JOP_SHIFT_RIGHT_UNSIGNED] = HIR_RUSHIFT,
  [JOP_SHIFT_RIGHT_UNSIGNED_IMMEDIATE] = HIR_RUSHIFT_IMM,
  /* [JOP_MOVE_FAR] = HIR_MOVE, */
  /* [JOP_MOVE_NEAR] = HIR_MOVE, */
  [JOP_JUMP] = HIR_JUMP,
  [JOP_JUMP_IF] = HIR_JUMP,
  [JOP_JUMP_IF_NOT] = HIR_JUMP,
  [JOP_JUMP_IF_NIL] = HIR_JUMP,
  [JOP_JUMP_IF_NOT_NIL] = HIR_JUMP,
  [JOP_GREATER_THAN] = HIR_GREATER_THAN,
  [JOP_GREATER_THAN_IMMEDIATE] = HIR_GREATER_THAN_IMM,
  [JOP_LESS_THAN] = HIR_LESS_THAN,
  [JOP_LESS_THAN_IMMEDIATE] = HIR_LESS_THAN_IMM,
  [JOP_EQUALS] = HIR_EQUALS,
  [JOP_EQUALS_IMMEDIATE] = HIR_EQUALS,
  [JOP_COMPARE] = HIR_COMPARE,
  [JOP_LOAD_NIL] = HIR_LOAD,
  [JOP_LOAD_TRUE] = HIR_LOAD,
  [JOP_LOAD_FALSE] = HIR_LOAD,
  [JOP_LOAD_INTEGER] = HIR_LOAD,
  [JOP_LOAD_CONSTANT] = HIR_LOAD,
  [JOP_LOAD_UPVALUE] = HIR_LOAD,
  [JOP_LOAD_SELF] = HIR_LOAD,
  /* [JOP_SET_UPVALUE] = HIR_SET_UPVALUE, */
  /* [JOP_CLOSURE] = HIR_CLOSURE, */
  [JOP_PUSH] = HIR_PUSH,
  [JOP_PUSH_2] = HIR_PUSH,
  [JOP_PUSH_3] = HIR_PUSH,
  [JOP_PUSH_ARRAY] = HIR_PUSH,
  [JOP_CALL] = HIR_CALL,
  [JOP_TAILCALL] = HIR_TAIL_CALL,
  /* [JOP_RESUME] = HIR_RESUME, */
  /* [JOP_SIGNAL] = HIR_SIGNAL, */
  /* [JOP_PROPAGATE] = HIR_PROPAGATE, */
  [JOP_IN] = HIR_IN,
  [JOP_GET] = HIR_GET,
  [JOP_PUT] = HIR_PUT,
  [JOP_GET_INDEX] = HIR_GET,
  [JOP_PUT_INDEX] = HIR_PUT,
  [JOP_LENGTH] = HIR_LENGTH,
  [JOP_MAKE_ARRAY] = HIR_MAKE_ARRAY,
  [JOP_MAKE_BUFFER] = HIR_MAKE_BUFFER,
  [JOP_MAKE_STRING] = HIR_MAKE_STRING,
  [JOP_MAKE_STRUCT] = HIR_MAKE_STRUCT,
  [JOP_MAKE_TABLE] = HIR_MAKE_TABLE,
  [JOP_MAKE_TUPLE] = HIR_MAKE_TUPLE,
  [JOP_MAKE_BRACKET_TUPLE] = HIR_MAKE_BRACKET_TUPLE,
  [JOP_GREATER_THAN_EQUAL] = HIR_GREATER_THAN_EQUAL,
  [JOP_LESS_THAN_EQUAL] = HIR_LESS_THAN_EQUAL,
  [JOP_NEXT] = HIR_NEXT,
  [JOP_NOT_EQUALS] = HIR_NOT_EQUALS,
  [JOP_NOT_EQUALS_IMMEDIATE] = HIR_NOT_EQUALS_IMM,
  /* [JOP_CANCEL] = HIR_CANCEL, */
};

typedef struct {
  size_t id;
  InstructionType type;
  size_t result;
  size_t args[3];

  size_t phi_placeholder_slot;
  size_t phi_source_count;
  size_t phi_source_capacity;
  PhiSource *phi_sources;
} Instruction;

// This is silly as a struct
typedef struct {
  size_t basic_block_id;
} Edge;

struct BasicBlock {
  bool complete;
  size_t start_pc;
  size_t finish_pc;
  size_t count;
  size_t capacity;
  Instruction *instructions;

  // TODO: ADAM start here
  uint32_t *virtual_register_def;
  uint32_t *virtual_register_last_use;

  // TODO: this is only valid while reading the block so no reason to keep around
  uint32_t *slot_map;

  size_t input_edge_count;
  size_t input_edge_capacity;
  Edge *input_edges;

  size_t output_edge_count;
  size_t output_edge_capacity;
  Edge *output_edges;

  bool *slot_use;
  bool *slot_def;
  bool *live_in;
  bool *live_out;
};

typedef struct {
  size_t count;
  size_t capacity;
  BasicBlock *blocks;
  size_t *block_start_pcs; // leader janet pc -> block_id
  size_t *block_locations; // block_id -> location in x86 assembly

  size_t vreg_count;
  size_t vreg_capacity;
  Operand *vregs;

  size_t imm_count;
  size_t imm_capacity;
  Operand *imms;

  size_t bb_count;
  size_t bb_capacity;
  Operand *bbs;

  // TODO: move the type info onto the operand itself
  uint32_t *virtual_register_types;
  uint32_t *vreg_defs;
  uint32_t *vreg_last_uses;
} MethodBlocks;

typedef struct {
  size_t count;
  size_t capacity;
  uint8_t *data;

  size_t jump_index;
  size_t jump_capacity;
  size_t *jump_targets;
  size_t *jump_locations;
} CodeBuffer;


typedef struct {
  size_t count;
  size_t argv_offset;
  size_t capacity;
  Janet *argv;
} CallArgs;

typedef enum {
  MISMATCH_ERROR,
  MISMATCH_FALLBACK,
  MISMATCH_RECOMPILE,
} MismatchBehavior;

typedef struct {
  size_t code_size;
  void *code;
  MismatchBehavior mismatch_behavior;
  JanetFunction *fallback;
  int32_t signature_argc;
  JanetType *signature_arg_types;
  MethodBlocks method_blocks;
  CallArgs ca;
} JittedFunction;

void setup_method_blocks(MethodBlocks *blocks, size_t bytecode_length) {
  blocks->count = 0;
  blocks->capacity = 256;
  blocks->blocks = malloc(blocks->capacity * sizeof(BasicBlock));
  blocks->block_start_pcs = malloc(bytecode_length * sizeof(size_t));

  blocks->block_locations = malloc(bytecode_length * sizeof(size_t));

  blocks->vreg_count = 1;
  blocks->vreg_capacity = 64;
  blocks->vregs = calloc(blocks->vreg_capacity, sizeof(Operand));
  // 0 is placeholder unused operand
  blocks->vregs[0].type = OPERAND_UNUSED;

  blocks->imm_count = 0;
  blocks->imm_capacity = 64;
  blocks->imms = calloc(blocks->imm_capacity, sizeof(Operand));

  blocks->bb_count = 0;
  blocks->bb_capacity = 64;
  blocks->bbs = calloc(blocks->bb_capacity, sizeof(Operand));
}

#define CUR_BLOCK blocks->blocks[block_id]

Instruction *add_instruction(MethodBlocks *blocks, size_t block_id, InstructionType type) {
  if (CUR_BLOCK.count >= CUR_BLOCK.capacity) {
    if (CUR_BLOCK.capacity < SIZE_MAX) {
      CUR_BLOCK.capacity *= 2;
      CUR_BLOCK.instructions = realloc(CUR_BLOCK.instructions, CUR_BLOCK.capacity * sizeof(Instruction));
      if (CUR_BLOCK.instructions == NULL) {
	janet_panic("JIT could not allocate while generating instructions");
      }
    } else {
      janet_panic("JIT ran out of memory generating instructions");
    }
  }

  Instruction *instruction = &CUR_BLOCK.instructions[CUR_BLOCK.count];

  instruction->result = 0;
  instruction->args[0] = 0;
  instruction->args[1] = 0;
  instruction->args[2] = 0;

  instruction->type = type;
  instruction->phi_source_count = 0;
  instruction->phi_source_capacity = 0;
  instruction->phi_sources = NULL;
  CUR_BLOCK.count++;
  return instruction;
}

#undef CUR_BLOCK

void add_edge(MethodBlocks *blocks, size_t source_bb, size_t target_bb) {
  BasicBlock *source_block = &blocks->blocks[source_bb];
  BasicBlock *target_block = &blocks->blocks[target_bb];
  if (source_block->output_edge_count >= source_block->output_edge_capacity) {
    if (source_block->output_edge_capacity * 2 < SIZE_MAX) {
      source_block->output_edge_capacity *= 2;
      source_block->output_edges = realloc(source_block->output_edges, source_block->output_edge_capacity * sizeof(Edge));
      if (source_block->output_edges == NULL) {
	janet_panic("JIT could not allocate while generating edges");
      }
    } else {
      janet_panic("JIT ran out of memory generating edges");
    }
  }

  if (target_block->input_edge_count >= target_block->input_edge_capacity) {
    if (target_block->input_edge_capacity * 2 < SIZE_MAX) {
      target_block->input_edge_capacity *= 2;
      target_block->input_edges = realloc(target_block->input_edges, target_block->input_edge_capacity * sizeof(Edge));
      if (target_block->input_edges == NULL) {
	janet_panic("JIT could not allocate while generating edges");
      }
    } else {
      janet_panic("JIT ran out of memory generating edges");
    }
  }

  blocks->blocks[source_bb].output_edges[blocks->blocks[source_bb].output_edge_count++].basic_block_id = target_bb;
  blocks->blocks[target_bb].input_edges[blocks->blocks[target_bb].input_edge_count++].basic_block_id = source_bb;
}

size_t new_basic_block(MethodBlocks *blocks, size_t slotcount, size_t start_pc) {
  if (blocks->count >= blocks->capacity) {
    if (blocks->count * 2 < SIZE_MAX) {
      blocks->capacity *= 2;
      blocks->blocks = realloc(blocks->blocks, blocks->capacity * sizeof(BasicBlock));
      if (blocks->blocks == NULL) {
	janet_panic("JIT could not allocate while generating basic blocks");
      }
    } else {
      janet_panic("JIT ran out of memory generating basic blocks");
    }
  }

  size_t block_id = blocks->count;
  blocks->count++;

  BasicBlock *block = &blocks->blocks[block_id];
  *block = (BasicBlock) {0};
  block->complete = false;
  block->start_pc = start_pc;
  block->capacity = 256;
  block->instructions = malloc(block->capacity * sizeof(Instruction));

  block->slot_map = malloc(slotcount * sizeof(uint32_t));
  for (size_t i = 0; i < slotcount; i++) {
    block->slot_map[i] = UINT32_MAX;
  }

  block->slot_use = calloc(slotcount, sizeof(bool));
  block->slot_def = calloc(slotcount, sizeof(bool));
  block->live_in =  calloc(slotcount, sizeof(bool));
  block->live_out = calloc(slotcount, sizeof(bool));

  block->input_edge_capacity = 12;
  block->input_edges = malloc(block->input_edge_capacity * sizeof(Edge));
  block->output_edge_capacity = 12;
  block->output_edges = malloc(block->output_edge_capacity * sizeof(Edge));
  return block_id;
}

#define AA ((instr >> 8)  & 0xFF)
#define BB ((instr >> 16) & 0xFF)
#define CC (instr >> 24)
#define DD (instr >> 8)
#define EE (instr >> 16)
#define CS (((int32_t)instr) >> 24)
#define DS (((int32_t)instr) >> 8)
#define ES (((int32_t)instr) >> 16)

static void add_slot_use(BasicBlock *block, size_t slot) {
  if (!block->slot_def[slot]) {
    block->slot_use[slot] = true;
  }
}

static void add_slot_def(BasicBlock *block, size_t slot) {
  block->slot_def[slot] = true;
}

static void ensure_operand_capacity(Operand **operands,
				    size_t *capacity,
				    size_t count,
				    const char *kind) {
  if (count < *capacity) {
    return;
  }

  if (*capacity > SIZE_MAX / 2 ||
      *capacity * 2 > SIZE_MAX / sizeof(Operand)) {
    janet_panicf("JIT ran out of memory generating %s operands", kind);
  }

  size_t old_capacity = *capacity;
  size_t new_capacity = old_capacity * 2;
  Operand *new_operands =
    realloc(*operands, new_capacity * sizeof(Operand));
  if (new_operands == NULL) {
    janet_panicf("JIT could not allocate while generating %s operands", kind);
  }

  memset(new_operands + old_capacity,
	 0,
	 (new_capacity - old_capacity) * sizeof(Operand));
  *operands = new_operands;
  *capacity = new_capacity;
}

static size_t new_vreg(MethodBlocks *blocks) {
  ensure_operand_capacity(&blocks->vregs,
			  &blocks->vreg_capacity,
			  blocks->vreg_count,
			  "virtual register");

  size_t index = blocks->vreg_count++;
  blocks->vregs[index] = (Operand) {
    .type = OPERAND_VIRTUAL_REGISTER,
    .virtual_register = index,
    .physical_location = {false, 0}
  };
  return index;
}

static size_t new_immus(MethodBlocks *blocks, uint32_t val) {
  ensure_operand_capacity(&blocks->imms,
			  &blocks->imm_capacity,
			  blocks->imm_count,
			  "immediate");

  size_t index = blocks->imm_count++;
  blocks->imms[index] = (Operand) {
    .type = OPERAND_UNSIGNED_IMM,
    .immus = val,
  };
  return index;
}

static size_t new_bb(MethodBlocks *blocks, uint32_t val) {
  ensure_operand_capacity(&blocks->bbs,
			  &blocks->bb_capacity,
			  blocks->bb_count,
			  "basic block");

  size_t index = blocks->bb_count++;
  blocks->bbs[index] = (Operand) {
    .type = OPERAND_BASIC_BLOCK,
    .bb = val,
  };
  return index;
}

static size_t new_imms(MethodBlocks *blocks, int32_t val) {
  ensure_operand_capacity(&blocks->imms,
			  &blocks->imm_capacity,
			  blocks->imm_count,
			  "immediate");

  size_t index = blocks->imm_count++;
  blocks->imms[index] = (Operand) {
    .type = OPERAND_SIGNED_IMM,
    .imms = val,
  };
  return index;
}

static size_t new_jimm(MethodBlocks *blocks, Janet val) {
  ensure_operand_capacity(&blocks->imms,
			  &blocks->imm_capacity,
			  blocks->imm_count,
			  "immediate");

  size_t index = blocks->imm_count++;
  blocks->imms[index] = (Operand) {
    .type = OPERAND_JANET_IMM,
    .immus = janet_u64(val),
  };
  return index;
}

static void compile_bb_bytecode(JittedFunction *jitted, JanetFunction *fn, size_t block_id, uint32_t *parent_slot_map) {
  MethodBlocks *blocks = &jitted->method_blocks;
  JanetFuncDef *def = fn->def;

  // keep my own copy of slot_map to avoid changing the owners copy
  uint32_t *slot_map = malloc(def->slotcount * sizeof(uint32_t));
  if (blocks->blocks[block_id].input_edge_count == 0) {
    memcpy(slot_map, parent_slot_map, def->slotcount * sizeof(uint32_t));
  } else {
    size_t input_block_id = blocks->blocks[block_id].input_edges[0].basic_block_id;
    memcpy(slot_map, blocks->blocks[input_block_id].slot_map, def->slotcount * sizeof(uint32_t));

    // emit phi for each change slot where the input edges can have different values
    for (size_t slot_i = 0; slot_i < def->slotcount; slot_i++) {
      if (!blocks->blocks[block_id].live_in[slot_i]) {
	continue;
      }

      bool found_difference = false;
      for (size_t edge_i = 1; edge_i < blocks->blocks[block_id].input_edge_count; edge_i++) {
	size_t input_block_id = blocks->blocks[block_id].input_edges[edge_i].basic_block_id;
	if (blocks->blocks[input_block_id].slot_map[slot_i] != blocks->blocks[blocks->blocks[block_id].input_edges[0].basic_block_id].slot_map[slot_i]) {
	  found_difference = true;
	}
      }

      if (found_difference) {
	Instruction *instruction = add_instruction(blocks, block_id, HIR_PHI_PLACEHOLDER);
	instruction->phi_source_count = blocks->blocks[block_id].input_edge_count;
	instruction->phi_source_capacity = blocks->blocks[block_id].input_edge_count;
	instruction->phi_sources = malloc(blocks->blocks[block_id].input_edge_count * sizeof(PhiSource));

	for (size_t edge_i = 0; edge_i < blocks->blocks[block_id].input_edge_count; edge_i++) {
	  size_t input_block_id = blocks->blocks[block_id].input_edges[edge_i].basic_block_id;

	  instruction->phi_sources[edge_i].bb = input_block_id;
	  instruction->phi_sources[edge_i].virtual_register = blocks->blocks[input_block_id].slot_map[slot_i];
	  instruction->phi_sources[edge_i].slot = slot_i;
	}

	instruction->result = new_vreg(blocks);
	slot_map[slot_i] = instruction->result;
      }
    }
  }

  free(blocks->blocks[block_id].slot_map);
  blocks->blocks[block_id].slot_map = slot_map;

  blocks->blocks[block_id].complete = true;
  for (size_t index = blocks->blocks[block_id].start_pc; index < blocks->blocks[block_id].finish_pc; index++) {
    uint32_t instr = def->bytecode[index];
    switch (instr & 0x7F) {
    case JOP_NOOP:
      break;
    case JOP_ERROR: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_ERROR);
      instruction->args[0] = slot_map[AA]; // vreg_for(blocks, slot_map[AA]);
      return;
    }
    case JOP_TYPECHECK: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_TYPECHECK);
      instruction->args[0] = slot_map[AA];
      instruction->args[1] = new_immus(blocks, EE);
      break;
    }
    case JOP_RETURN: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_RETURN);
      instruction->args[0] = slot_map[DD];
      return;
    }
    case JOP_RETURN_NIL: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_RETURN_NIL);
      instruction->args[0] = new_jimm(blocks, janet_wrap_nil());
      return;
    }
    case JOP_ADD_IMMEDIATE:
    case JOP_SUBTRACT_IMMEDIATE:
    case JOP_MULTIPLY_IMMEDIATE:
    case JOP_DIVIDE_IMMEDIATE:
    case JOP_SHIFT_RIGHT_IMMEDIATE:
    case JOP_SHIFT_LEFT_IMMEDIATE:
    case JOP_SHIFT_RIGHT_UNSIGNED_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, jop_to_hir[instr & 0x7F]);
      instruction->args[0] = slot_map[BB];
      instruction->args[1] = new_imms(blocks, CS);
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_ADD:
    case JOP_SUBTRACT:
    case JOP_MULTIPLY:
    case JOP_DIVIDE:
    case JOP_DIVIDE_FLOOR:
    case JOP_MODULO:
    case JOP_REMAINDER:
    case JOP_BAND:
    case JOP_BOR:
    case JOP_BXOR:
    case JOP_SHIFT_LEFT:
    case JOP_SHIFT_RIGHT:
    case JOP_SHIFT_RIGHT_UNSIGNED: {
      Instruction *instruction = add_instruction(blocks, block_id, jop_to_hir[instr & 0x7F]);
      instruction->args[0] = slot_map[BB];
      instruction->args[1] = slot_map[CC];
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_BNOT: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_NOT);
      instruction->args[0] = slot_map[EE];
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_MOVE_FAR: {
      slot_map[EE] = slot_map[AA];
      break;
    }
    case JOP_MOVE_NEAR: {
      slot_map[AA] = slot_map[EE];
      break;
    }
    case JOP_JUMP: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_JUMP);
      instruction->args[0] = new_bb(blocks, blocks->block_start_pcs[index + (((int32_t)instr) >> 8)]);
      return;
    }
    case JOP_JUMP_IF: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_JUMP_IF);
      instruction->args[2] = slot_map[AA];
      instruction->args[0] = new_bb(blocks, blocks->block_start_pcs[index + (((int32_t)instr) >> 16)]);
      instruction->args[1] = new_bb(blocks, blocks->block_start_pcs[index + 1]);
      return;
    }
    case JOP_JUMP_IF_NOT: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_JUMP_IF_NOT);
      instruction->args[2] = slot_map[AA];
      instruction->args[0] = new_bb(blocks, blocks->block_start_pcs[index + (((int32_t)instr) >> 16)]);
      instruction->args[1] = new_bb(blocks, blocks->block_start_pcs[index + 1]);
      return;
    }
    case JOP_JUMP_IF_NIL: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_JUMP_IF_NIL);
      instruction->args[2] = slot_map[AA];
      instruction->args[0] = new_bb(blocks, blocks->block_start_pcs[index + (((int32_t)instr) >> 16)]);
      instruction->args[1] = new_bb(blocks, blocks->block_start_pcs[index + 1]);
      return;
    }
    case JOP_JUMP_IF_NOT_NIL: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_JUMP_IF_NOT_NIL);
      instruction->args[2] = slot_map[AA];
      instruction->args[0] = new_bb(blocks, blocks->block_start_pcs[index + (((int32_t)instr) >> 16)]);
      instruction->args[1] = new_bb(blocks, blocks->block_start_pcs[index + 1]);
      return;
    }
    case JOP_GREATER_THAN: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_GREATER_THAN);
      instruction->args[0] = slot_map[BB];
      instruction->args[1] = slot_map[CC];
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_GREATER_THAN_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_GREATER_THAN_IMM);
      instruction->args[0] = slot_map[BB];
      instruction->args[1] = new_imms(blocks, CS);
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_LESS_THAN: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LESS_THAN);
      instruction->args[0] = slot_map[BB];
      instruction->args[1] = slot_map[CC];
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_LESS_THAN_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LESS_THAN_IMM);
      instruction->args[0] = slot_map[BB];
      instruction->args[1] = new_imms(blocks, CS);
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_EQUALS: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_EQUALS);
      instruction->args[0] = slot_map[BB];
      instruction->args[1] = slot_map[CC];
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_EQUALS_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_EQUALS_IMM);
      instruction->args[0] = slot_map[BB];
      instruction->args[1] = new_imms(blocks, CS);
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_COMPARE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_COMPARE);
      instruction->args[0] = slot_map[BB];
      instruction->args[1] = slot_map[CC];
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_LOAD_NIL: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LOAD);
      instruction->args[0] = new_jimm(blocks, janet_wrap_nil());
      instruction->result = new_vreg(blocks);
      slot_map[DD] = instruction->result;
      break;
    }
    case JOP_LOAD_TRUE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LOAD);
      instruction->args[0] = new_jimm(blocks, janet_wrap_true());
      instruction->result = new_vreg(blocks);
      slot_map[DD] = instruction->result;
      break;
    }
    case JOP_LOAD_FALSE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LOAD);
      instruction->args[0] = new_jimm(blocks, janet_wrap_false());
      instruction->result = new_vreg(blocks);
      slot_map[DD] = instruction->result;
      break;
    }
    case JOP_LOAD_INTEGER: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LOAD);
      instruction->args[0] = new_imms(blocks, ES);
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_LOAD_CONSTANT: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LOAD);
      instruction->args[0] = new_jimm(blocks, def->constants[EE]);
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_LOAD_UPVALUE: {
      janet_panic("janet's upvalue opcode is not supported");
      break;
    }
    case JOP_LOAD_SELF: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LOAD);
      instruction->args[0] = new_jimm(blocks, janet_wrap_abstract(jitted));
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_SET_UPVALUE: {
      janet_panic("janet's set upvalue opcode is not supported");
      break;
    }
    case JOP_CLOSURE: {
      janet_panic("janet's closure opcode is not supported");
      break;
    }
    case JOP_PUSH: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_PUSH);
      instruction->args[0] = slot_map[DD];
      break;
    }
    case JOP_PUSH_2: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_PUSH2);
      instruction->args[0] = slot_map[AA];
      instruction->args[1] = slot_map[EE];
      break;
    }
    case JOP_PUSH_3: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_PUSH3);
      instruction->args[0] = slot_map[AA];
      instruction->args[1] = slot_map[BB];
      instruction->args[2] = slot_map[CC];
      break;
    }
    case JOP_PUSH_ARRAY: {
      janet_panic("janet's push array is not yet supported");
      break;
    }
    case JOP_CALL: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_CALL);
      instruction->args[0] = slot_map[EE];
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_TAILCALL: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_TAIL_CALL);
      instruction->args[0] = slot_map[DD];
      return;
    }
    case JOP_RESUME: {
      janet_panic("janet's resume is not supported");
      break;
    }
    case JOP_SIGNAL: {
      janet_panic("janet's signal is not supported");
      break;
    }
    case JOP_PROPAGATE: {
      janet_panic("janet's propagate is not supported");
      break;
    }
    case JOP_IN: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_IN);
      instruction->args[0] = slot_map[BB];
      instruction->args[1] = slot_map[CC];
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_GET: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_GET);
      instruction->args[0] = slot_map[BB];
      instruction->args[1] = slot_map[CC];
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_PUT: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_PUT);
      instruction->args[0] = slot_map[AA];
      instruction->args[1] = slot_map[BB];
      instruction->args[2] = slot_map[CC];
      break;
    }
    case JOP_GET_INDEX: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_GET_INDEX);
      instruction->args[0] = slot_map[BB];
      instruction->args[1] = new_immus(blocks, CC);
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_PUT_INDEX: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_PUT_INDEX);
      instruction->args[0] = slot_map[AA];
      instruction->args[1] = new_immus(blocks, CC);
      instruction->args[2] = slot_map[BB];
      break;
    }
    case JOP_LENGTH: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LENGTH);
      instruction->args[0] = slot_map[EE];
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_MAKE_ARRAY: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_MAKE_ARRAY);
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_MAKE_BUFFER: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_MAKE_BUFFER);
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_MAKE_STRING: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_MAKE_STRING);
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_MAKE_STRUCT: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_MAKE_STRUCT);
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_MAKE_TABLE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_MAKE_TABLE);
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_MAKE_TUPLE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_MAKE_TUPLE);
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_MAKE_BRACKET_TUPLE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_MAKE_BRACKET_TUPLE);
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_GREATER_THAN_EQUAL: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_GREATER_THAN_EQUAL);
      instruction->args[0] = slot_map[BB];
      instruction->args[1] = slot_map[CC];
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_LESS_THAN_EQUAL: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LESS_THAN_EQUAL);
      instruction->args[0] = slot_map[BB];
      instruction->args[1] = slot_map[CC];
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_NEXT: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_NEXT);
      instruction->args[0] = slot_map[BB];
      instruction->args[1] = slot_map[CC];
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_NOT_EQUALS: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_NOT_EQUALS);
      instruction->args[0] = slot_map[BB];
      instruction->args[1] = slot_map[CC];
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_NOT_EQUALS_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_NOT_EQUALS_IMM);
      instruction->args[0] = slot_map[BB];
      instruction->args[1] = new_imms(blocks, CC);
      instruction->result = new_vreg(blocks);
      slot_map[AA] = instruction->result;
      break;
    }
    case JOP_CANCEL: {
      janet_panic("janet's resume is not supported");
      break;
    }
    }
  }
  // reached end due to leader / jump target instruction
  Instruction *instruction = add_instruction(blocks, block_id, HIR_JUMP);
  instruction->args[0] = new_bb(blocks, blocks->block_start_pcs[blocks->blocks[block_id].finish_pc]);
}

static void rpo_order(MethodBlocks *blocks, size_t block_id, bool *visited, size_t *list, size_t *count) {
  if (visited[block_id]) { return; }
  visited[block_id] = true;
  for (size_t i = 0; i < blocks->blocks[block_id].output_edge_count; i++) {
    rpo_order(blocks, blocks->blocks[block_id].output_edges[i].basic_block_id, visited, list, count);
  }

  list[(*count)++] = block_id;
}

#define VREG_ARG0 (1u << 0)
#define VREG_ARG1 (1u << 1)
#define VREG_ARG2 (1u << 2)

typedef struct {
  uint8_t vregs;
  uint8_t imms;
  uint8_t bbs;
} InstructionArgs;

// TODO: all usage of this can be replaced with vreg_count
// for each instruction type
static InstructionArgs instruction_args(InstructionType type) {
  switch (type) {
  case HIR_ERROR:
  case HIR_RETURN:
  case HIR_NOT:
  case HIR_PUSH:
  case HIR_CALL:
  case HIR_TAIL_CALL:
  case HIR_LENGTH:
    return (InstructionArgs) {
      .vregs = VREG_ARG0,
    };

  case HIR_ADD:
  case HIR_SUB:
  case HIR_MUL:
  case HIR_DIV:
  case HIR_DIV_FLOOR:
  case HIR_MODULO:
  case HIR_REMAINDER:
  case HIR_AND:
  case HIR_OR:
  case HIR_XOR:
  case HIR_LSHIFT:
  case HIR_RSHIFT:
  case HIR_RUSHIFT:
  case HIR_GREATER_THAN:
  case HIR_LESS_THAN:
  case HIR_EQUALS:
  case HIR_COMPARE:
  case HIR_PUSH2:
  case HIR_IN:
  case HIR_GET:
  case HIR_GREATER_THAN_EQUAL:
  case HIR_LESS_THAN_EQUAL:
  case HIR_NEXT:
  case HIR_NOT_EQUALS:
    return (InstructionArgs) {
      .vregs = VREG_ARG0 | VREG_ARG1,
    };

  case HIR_TYPECHECK:
  case HIR_ADD_IMM:
  case HIR_SUB_IMM:
  case HIR_MUL_IMM:
  case HIR_DIV_IMM:
  case HIR_LSHIFT_IMM:
  case HIR_RSHIFT_IMM:
  case HIR_RUSHIFT_IMM:
  case HIR_GREATER_THAN_IMM:
  case HIR_LESS_THAN_IMM:
  case HIR_EQUALS_IMM:
  case HIR_GET_INDEX:
  case HIR_NOT_EQUALS_IMM:
    return (InstructionArgs) {
      .vregs = VREG_ARG0,
      .imms = VREG_ARG1,
    };

  case HIR_RETURN_NIL:
  case HIR_LOAD:
  case HIR_LOAD_ARG:
    return (InstructionArgs) {
      .imms = VREG_ARG0,
    };

  case HIR_JUMP:
    return (InstructionArgs) {
      .bbs = VREG_ARG0,
    };

  case HIR_JUMP_IF:
  case HIR_JUMP_IF_NOT:
  case HIR_JUMP_IF_NIL:
  case HIR_JUMP_IF_NOT_NIL:
    return (InstructionArgs) {
      .vregs = VREG_ARG2,
      .bbs = VREG_ARG0 | VREG_ARG1,
    };

  case HIR_PUT_INDEX:
    return (InstructionArgs) {
      .vregs = VREG_ARG0 | VREG_ARG2,
      .imms = VREG_ARG1,
    };

  case HIR_PUSH3:
  case HIR_PUT:
    return (InstructionArgs) {
      .vregs = VREG_ARG0 | VREG_ARG1 | VREG_ARG2,
    };

  default:
    return (InstructionArgs) {0};
  }
}

static uint8_t instruction_vreg_args(InstructionType type) {
  return instruction_args(type).vregs;
}

static void replace_virtual_register(MethodBlocks *blocks,
				     size_t slotcount,
				     size_t old_vreg,
				     size_t new_vreg) {
  if (old_vreg == new_vreg) {
    return;
  }

  for (size_t block_i = 0; block_i < blocks->count; block_i++) {
    BasicBlock *block = &blocks->blocks[block_i];

    for (size_t slot_i = 0; slot_i < slotcount; slot_i++) {
      if (block->slot_map[slot_i] == old_vreg) {
	block->slot_map[slot_i] = new_vreg;
      }
    }

    for (size_t instr_i = 0; instr_i < block->count; instr_i++) {
      Instruction *instr = &block->instructions[instr_i];
      uint8_t vreg_args = instruction_vreg_args(instr->type);

      for (size_t arg_i = 0; arg_i < 3; arg_i++) {
	if ((vreg_args & (1u << arg_i)) &&
	    instr->args[arg_i] == old_vreg) {
	  instr->args[arg_i] = new_vreg;
	}
      }

      if (instr->type == HIR_PHI ||
	  instr->type == HIR_PHI_PLACEHOLDER) {
	for (size_t source_i = 0;
	     source_i < instr->phi_source_count;
	     source_i++) {
	  PhiSource *source = &instr->phi_sources[source_i];
	  if (source->virtual_register == old_vreg) {
	    source->virtual_register = new_vreg;
	  }
	}
      }
    }
  }
}

#undef VREG_ARG0
#undef VREG_ARG1
#undef VREG_ARG2

void build_basic_blocks(JittedFunction *jitted, JanetFunction *fn) {
  MethodBlocks *blocks = &jitted->method_blocks;
  // mark leaders
  bool *leaders = calloc(fn->def->bytecode_length, sizeof(bool));
  leaders[0] = true;
  for (int i = 0; i < fn->def->bytecode_length; i++) {
    uint32_t instr = fn->def->bytecode[i];
    switch (instr & 0x7F) {
    case JOP_JUMP: {
      leaders[i + ((int32_t)instr >> 8)] = true;
      leaders[i + 1] = true;
      break;
    }
    case JOP_JUMP_IF:
    case JOP_JUMP_IF_NOT:
    case JOP_JUMP_IF_NIL:
    case JOP_JUMP_IF_NOT_NIL: {
      leaders[i + ((int32_t)instr >> 16)] = true;
      leaders[i + 1] = true;
      break;
    }
    }
  }

  // create basic blocks
  int32_t cur_block = -1;
  for (int pc = 0; pc < fn->def->bytecode_length; pc++) {
    blocks->block_start_pcs[pc] = -1;
    if (leaders[pc]) {
      blocks->block_start_pcs[pc] = new_basic_block(blocks, fn->def->slotcount, pc);
      if (cur_block >= 0) {
	blocks->blocks[cur_block].finish_pc = pc;
      }
      cur_block = blocks->block_start_pcs[pc];
    }
  }
  if (cur_block >= 0) {
    blocks->blocks[cur_block].finish_pc = fn->def->bytecode_length;
  }

  // add edges between basic blocks
  for (size_t block_id = 0; block_id < blocks->count; block_id++) {
    uint32_t last_op = blocks->blocks[block_id].finish_pc - 1;
    uint64_t instr = fn->def->bytecode[last_op];
    switch (instr & 0x7F) {
    case JOP_JUMP: {
      add_edge(blocks, block_id, blocks->block_start_pcs[last_op + ((int32_t)instr >> 8)]);
      break;
    }
    case JOP_JUMP_IF:
    case JOP_JUMP_IF_NOT:
    case JOP_JUMP_IF_NIL:
    case JOP_JUMP_IF_NOT_NIL: {
      add_edge(blocks, block_id, blocks->block_start_pcs[last_op + ((int32_t)instr >> 16)]);
      add_edge(blocks, block_id, blocks->block_start_pcs[last_op + 1]);
      break;
    }
    case JOP_RETURN:
    case JOP_RETURN_NIL:
    case JOP_ERROR:
    case JOP_TAILCALL:
      break;
    default:
      add_edge(blocks, block_id, blocks->block_start_pcs[last_op + 1]);
    }
  }

  // liveness use and def
  for (size_t block_id = 0; block_id < blocks->count; block_id++) {
    BasicBlock *block = &blocks->blocks[block_id];

    for (size_t pc = block->start_pc; pc < block->finish_pc; pc++) {
      uint32_t instr = fn->def->bytecode[pc];
      switch (instr & 0x7F) {
      case JOP_NOOP:
      case JOP_RETURN_NIL:
      case JOP_JUMP:
	break;
      case JOP_ERROR:
      case JOP_TYPECHECK:
      case JOP_JUMP_IF:
      case JOP_JUMP_IF_NOT:
      case JOP_JUMP_IF_NIL:
      case JOP_JUMP_IF_NOT_NIL:
      case JOP_SET_UPVALUE:
	add_slot_use(block, AA);
	break;
      case JOP_RETURN:
      case JOP_PUSH:
      case JOP_PUSH_ARRAY:
      case JOP_TAILCALL:
	add_slot_use(block, DD);
	break;
      case JOP_LOAD_NIL:
      case JOP_LOAD_TRUE:
      case JOP_LOAD_FALSE:
      case JOP_LOAD_SELF:
      case JOP_MAKE_ARRAY:
      case JOP_MAKE_BUFFER:
      case JOP_MAKE_STRING:
      case JOP_MAKE_STRUCT:
      case JOP_MAKE_TABLE:
      case JOP_MAKE_TUPLE:
      case JOP_MAKE_BRACKET_TUPLE:
	add_slot_def(block, DD);
	break;
      case JOP_LOAD_INTEGER:
      case JOP_LOAD_CONSTANT:
      case JOP_LOAD_UPVALUE:
      case JOP_CLOSURE:
	add_slot_def(block, AA);
	break;
      case JOP_MOVE_FAR:
	add_slot_use(block, AA);
	add_slot_def(block, EE);
	break;
      case JOP_MOVE_NEAR:
      case JOP_BNOT:
      case JOP_LENGTH:
      case JOP_CALL:
	add_slot_use(block, EE);
	add_slot_def(block, AA);
	break;
      case JOP_PUSH_2:
	add_slot_use(block, AA);
	add_slot_use(block, EE);
	break;
      case JOP_PUSH_3:
      case JOP_PUT:
	add_slot_use(block, AA);
	add_slot_use(block, BB);
	add_slot_use(block, CC);
	break;
      case JOP_PUT_INDEX:
	add_slot_use(block, AA);
	add_slot_use(block, BB);
	break;
      case JOP_SIGNAL:
	add_slot_use(block, BB);
	add_slot_def(block, AA);
	break;
      case JOP_ADD_IMMEDIATE:
      case JOP_SUBTRACT_IMMEDIATE:
      case JOP_MULTIPLY_IMMEDIATE:
      case JOP_DIVIDE_IMMEDIATE:
      case JOP_SHIFT_LEFT_IMMEDIATE:
      case JOP_SHIFT_RIGHT_IMMEDIATE:
      case JOP_SHIFT_RIGHT_UNSIGNED_IMMEDIATE:
      case JOP_GREATER_THAN_IMMEDIATE:
      case JOP_LESS_THAN_IMMEDIATE:
      case JOP_EQUALS_IMMEDIATE:
      case JOP_NOT_EQUALS_IMMEDIATE:
      case JOP_GET_INDEX:
	add_slot_use(block, BB);
	add_slot_def(block, AA);
	break;
      case JOP_ADD:
      case JOP_SUBTRACT:
      case JOP_MULTIPLY:
      case JOP_DIVIDE:
      case JOP_DIVIDE_FLOOR:
      case JOP_MODULO:
      case JOP_REMAINDER:
      case JOP_BAND:
      case JOP_BOR:
      case JOP_BXOR:
      case JOP_SHIFT_LEFT:
      case JOP_SHIFT_RIGHT:
      case JOP_SHIFT_RIGHT_UNSIGNED:
      case JOP_GREATER_THAN:
      case JOP_LESS_THAN:
      case JOP_EQUALS:
      case JOP_COMPARE:
      case JOP_PROPAGATE:
      case JOP_IN:
      case JOP_GET:
      case JOP_GREATER_THAN_EQUAL:
      case JOP_LESS_THAN_EQUAL:
      case JOP_NEXT:
      case JOP_NOT_EQUALS:
      case JOP_CANCEL:
      case JOP_RESUME:
	add_slot_use(block, BB);
	add_slot_use(block, CC);
	add_slot_def(block, AA);
	break;
      default:
	janet_panicf("unsupported opcode in liveness analysis: %d", instr & 0x7F);
      }
    }
  }

  // live_in / live_out
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t block_i = blocks->count; block_i-- > 0;) {
      BasicBlock *block = &blocks->blocks[block_i];

      for (size_t slot_i = 0; slot_i < fn->def->slotcount; slot_i++) {
	bool live_out = false;
	for (size_t edge_i = 0; edge_i < block->output_edge_count; edge_i++) {
	  size_t successor_id = block->output_edges[edge_i].basic_block_id;
	  if (blocks->blocks[successor_id].live_in[slot_i]) {
	    live_out = true;
	    break;
	  }
	}

	bool live_in = block->slot_use[slot_i] ||
	  (live_out && !block->slot_def[slot_i]);
	if (block->live_out[slot_i] != live_out ||
	    block->live_in[slot_i] != live_in) {
	  block->live_out[slot_i] = live_out;
	  block->live_in[slot_i] = live_in;
	  changed = true;
	}
      }
    }
  }

  // parent slot map with just the args in it.
  // only used if a block has no edges pointing to it.
  uint32_t *slot_map = malloc(fn->def->slotcount * sizeof(uint32_t));
  for (int slot_i = 0; slot_i < fn->def->slotcount; slot_i++) {
    slot_map[slot_i] = UINT32_MAX;
  }
  for (int i = 0; i < fn->def->arity; i++) {
    Instruction *instruction = add_instruction(blocks, 0, HIR_LOAD_ARG);
    instruction->args[0] = new_immus(blocks, i);
    instruction->result = new_vreg(blocks);
    slot_map[i] = instruction->result;
  }

  // rpo order
  size_t *list = malloc(blocks->count * sizeof(size_t));
  bool *visited = calloc(blocks->count, sizeof(bool));
  size_t count = 0;
  rpo_order(blocks, 0, visited, list, &count);

  while (count > 0) {
    compile_bb_bytecode(jitted, fn, list[--count], slot_map);
  }
  free(slot_map);
  free(list);
  free(visited);

  // resolve phis
  changed = true;
  while (changed) {
    changed = false;
    for (size_t block_i = 0; block_i < blocks->count; block_i++) {
      BasicBlock *block = &blocks->blocks[block_i];
      for (size_t instr_i = 0; instr_i < block->count; instr_i++) {
	Instruction *instr = &block->instructions[instr_i];
	if (instr->type == HIR_PHI_PLACEHOLDER) {
	  instr->type = HIR_PHI;
	  for (size_t i = 0; i < instr->phi_source_count; i++) {
	    PhiSource *source = &instr->phi_sources[i];
	    size_t slot_i = source->slot;
	    size_t bb_i = source->bb;

	    if (blocks->blocks[bb_i].slot_map[slot_i] == UINT32_MAX) {
	      janet_panic("should be unreachable (unresolved live phi made it here)");
	    }
	    source->virtual_register = blocks->blocks[bb_i].slot_map[slot_i];
	  }
	}

	if (instr->type == HIR_PHI) {
	  bool found_source = false;
	  bool needs_phi = false;
	  size_t replacement = 0;
	  for (size_t i = 0; i < instr->phi_source_count; i++) {
	    PhiSource *source = &instr->phi_sources[i];

	    if (source->virtual_register == instr->result) {
	      continue;
	    }

	    if (!found_source) {
	      found_source = true;
	      replacement = source->virtual_register;
	    } else if (source->virtual_register != replacement) {
	      needs_phi = true;
	      break;
	    }
	  }

	  if (!found_source) {
	    instr->type = HIR_NOOP;
	  } else if (!needs_phi) {
	    size_t old_result = instr->result;
	    instr->type = HIR_NOOP;
	    instr->result = 0;
	    replace_virtual_register(blocks,
				     fn->def->slotcount,
				     old_result,
				     replacement);
	    changed = true;
	  }
	}
      }
    }
  }

  // TODO: technically, this number can be lowered if `replace_virtaul_register` has run.
  // but reducing the number would require renumbering.
  blocks->virtual_register_types = calloc(blocks->vreg_count, sizeof(uint32_t));

  // lower phis
  // TODO

  free(leaders);
}

static uint32_t operand_type(MethodBlocks *blocks, Operand *op) {
  switch (op->type) {
  case OPERAND_VIRTUAL_REGISTER:
    return blocks->virtual_register_types[op->virtual_register];
  case OPERAND_SIGNED_IMM:
  case OPERAND_UNSIGNED_IMM:
    return JANET_TFLAG_NUMBER;
  case OPERAND_JANET_IMM: {
    Janet value;
    value.u64 = op->immus;
    return 1u << janet_type(value);
  }
  }
}

#define JIT_JANET_TFLAG_ANY (UINT32_MAX >> (32 - JANET_COUNT_TYPES))

void add_type(MethodBlocks *blocks, uint32_t vreg, uint32_t tflag, bool *changed) {
  uint32_t cur_type = blocks->virtual_register_types[vreg];
  uint32_t updated_type = cur_type | tflag;
  if (cur_type == updated_type) {
    return;
  }
  (*changed) = true;
  blocks->virtual_register_types[vreg] |= tflag;
}

void type_flow(MethodBlocks *blocks, int32_t argc, Janet *argv) {
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t block_i = 0; block_i < blocks->count; block_i++) {
      BasicBlock *block = &blocks->blocks[block_i];
      for (size_t instr_i = 0; instr_i < block->count; instr_i++) {
	Instruction *instr = &block->instructions[instr_i];
	switch (instr->type) {
	case HIR_NOOP:
	case HIR_ERROR:
	case HIR_RETURN:
	case HIR_RETURN_NIL:
	case HIR_JUMP:
	case HIR_JUMP_IF:
	case HIR_JUMP_IF_NOT:
	case HIR_JUMP_IF_NIL:
	case HIR_JUMP_IF_NOT_NIL:
	case HIR_TAIL_CALL:
	case HIR_PUT:
	case HIR_PUT_INDEX:
	case HIR_PUSH:
	case HIR_PUSH2:
	case HIR_PUSH3:
	  // noop
	  break;
	case HIR_TYPECHECK:
	  // TODO: I can use this data, but I am ignoring it for now.
	  break;
	case HIR_ADD:
	case HIR_SUB:
	case HIR_MUL:
	case HIR_DIV:
	case HIR_DIV_FLOOR:
	case HIR_MODULO:
	case HIR_REMAINDER:
	case HIR_AND:
	case HIR_OR:
	case HIR_XOR:
	case HIR_LSHIFT:
	case HIR_RSHIFT:
	case HIR_RUSHIFT: {
	  uint32_t lhs = operand_type(blocks, &blocks->vregs[instr->args[0]]);
	  uint32_t rhs = operand_type(blocks, &blocks->vregs[instr->args[1]]);
	  if (lhs == 0 || rhs == 0) {
	    // too early to know
	  } else if (lhs == JANET_TFLAG_NUMBER && rhs == JANET_TFLAG_NUMBER) {
	    add_type(blocks, instr->result, JANET_TFLAG_NUMBER, &changed);
	  } else {
	    add_type(blocks, instr->result, JIT_JANET_TFLAG_ANY, &changed);
	  }
	  break;
	}
	case HIR_ADD_IMM:
	case HIR_SUB_IMM:
	case HIR_MUL_IMM:
	case HIR_DIV_IMM:
	case HIR_LSHIFT_IMM:
	case HIR_RSHIFT_IMM:
	case HIR_RUSHIFT_IMM: {
	  uint32_t lhs = operand_type(blocks, &blocks->vregs[instr->args[0]]);
	  if (lhs == 0) {
	    // too early to know
	  } else if (lhs == JANET_TFLAG_NUMBER) {
	    add_type(blocks, instr->result, JANET_TFLAG_NUMBER, &changed);
	  } else {
	    add_type(blocks, instr->result, JIT_JANET_TFLAG_ANY, &changed);
	  }
	  break;
	}
	case HIR_NOT: {
	  // op1 is alwasy a virtual register.
	  uint32_t val = operand_type(blocks, &blocks->vregs[instr->args[0]]);
	  if (val == 0) {
	    // too early to know
	  } else if (val == JANET_TFLAG_NUMBER) {
	    add_type(blocks, instr->result, JANET_TFLAG_NUMBER, &changed);
	  } else {
	    add_type(blocks, instr->result, JIT_JANET_TFLAG_ANY, &changed);
	  }
	  break;
	}
	case HIR_COMPARE:
	  add_type(blocks, instr->result, JANET_TFLAG_NUMBER, &changed);
	  break;
	case HIR_GREATER_THAN:
	case HIR_LESS_THAN:
	case HIR_EQUALS:
	case HIR_GREATER_THAN_IMM:
	case HIR_LESS_THAN_IMM:
	case HIR_EQUALS_IMM:
	case HIR_GREATER_THAN_EQUAL:
	case HIR_LESS_THAN_EQUAL:
	case HIR_NOT_EQUALS:
	  add_type(blocks, instr->result, JANET_TFLAG_BOOLEAN, &changed);
	  break;
	case HIR_LOAD:
	  add_type(blocks, instr->result, operand_type(blocks, &blocks->imms[instr->args[0]]), &changed);
	  break;
	case HIR_LOAD_ARG:
	  add_type(blocks, instr->result, 1u << janet_type(argv[blocks->imms[instr->args[0]].immus]), &changed);
	  break;
	case HIR_CALL:
	case HIR_IN:
	case HIR_GET:
	case HIR_GET_INDEX:
	case HIR_NEXT:
	  add_type(blocks, instr->result, JIT_JANET_TFLAG_ANY, &changed);
	  break;
	case HIR_LENGTH:
	  add_type(blocks, instr->result, JANET_TFLAG_NUMBER, &changed);
	  break;
	case HIR_MAKE_ARRAY:
	  add_type(blocks, instr->result, JANET_TFLAG_ARRAY, &changed);
	  break;
	case HIR_MAKE_BUFFER:
	  add_type(blocks, instr->result, JANET_TFLAG_BUFFER, &changed);
	  break;
	case HIR_MAKE_STRING:
	  add_type(blocks, instr->result, JANET_TFLAG_STRING, &changed);
	  break;
	case HIR_MAKE_STRUCT:
	  add_type(blocks, instr->result, JANET_TFLAG_STRUCT, &changed);
	  break;
	case HIR_MAKE_TABLE:
	  add_type(blocks, instr->result, JANET_TFLAG_TABLE, &changed);
	  break;
	case HIR_MAKE_TUPLE:
	case HIR_MAKE_BRACKET_TUPLE:
	  add_type(blocks, instr->result, JANET_TFLAG_TUPLE, &changed);
	  break;
	case HIR_PHI: {
	  for (size_t i = 0; i < instr->phi_source_count; i++) {
	    PhiSource *source = &instr->phi_sources[i];
	    add_type(blocks, instr->result, blocks->virtual_register_types[source->virtual_register], &changed);
	  }
	  break;
	}
	case HIR_PHI_PLACEHOLDER:
	  // TODO: assert unreachable
	  /* assert(false); */
	  break;
	}
      }
    }
  }
}

// I don't know how bad this is,
// but for now I am using a instruction_location based on the
// number of instructions since the start of the first basic block
// This assumes that basic blocks are output in the order they are received
// I think this is a fine assumption for now.
static bool instruction_defines_vreg(InstructionType type) {
  switch (type) {
  case HIR_NOOP:
  case HIR_ERROR:
  case HIR_TYPECHECK:
  case HIR_RETURN:
  case HIR_RETURN_NIL:
  case HIR_JUMP:
  case HIR_JUMP_IF:
  case HIR_JUMP_IF_NOT:
  case HIR_JUMP_IF_NIL:
  case HIR_JUMP_IF_NOT_NIL:
  case HIR_PUSH:
  case HIR_PUSH2:
  case HIR_PUSH3:
  case HIR_TAIL_CALL:
  case HIR_PUT:
  case HIR_PUT_INDEX:
    return false;
  default:
    return true;
  }
}

static void register_allocate(MethodBlocks *blocks) {
  // make our live ranges
  uint32_t *vreg_defs = calloc(blocks->vreg_count, sizeof(uint32_t));
  uint32_t *vreg_last_uses = calloc(blocks->vreg_count, sizeof(uint32_t));
  bool *vreg_defined = calloc(blocks->vreg_count, sizeof(bool));
  size_t instruction_location = 0;

  for (size_t block_i = 0; block_i < blocks->count; block_i++) {
    BasicBlock *block = &blocks->blocks[block_i];
    for (size_t instr_i = 0; instr_i < block->count; instr_i++) {
      block->instructions[instr_i].id = instruction_location++;
    }
  }

  for (size_t block_i = 0; block_i < blocks->count; block_i++) {
    BasicBlock *block = &blocks->blocks[block_i];
    for (size_t instr_i = 0; instr_i < block->count; instr_i++) {
      Instruction *instr = &block->instructions[instr_i];
      instruction_location = instr->id;
      if (instruction_defines_vreg(instr->type)) {
        vreg_defs[instr->result] = instruction_location;
        vreg_last_uses[instr->result] = instruction_location;
        vreg_defined[instr->result] = true;
      }

      if (instr->type == HIR_PHI) {
	for (size_t source_i = 0; source_i < instr->phi_source_count; source_i++) {
          PhiSource *source = &instr->phi_sources[source_i];
	  size_t vreg = source->virtual_register;
          BasicBlock *source_block = &blocks->blocks[source->bb];
          size_t use_location =
            source_block->instructions[source_block->count - 1].id;
	  if (vreg_last_uses[vreg] < use_location) {
            vreg_last_uses[vreg] = use_location;
          }
	}
      } else {
	uint8_t vreg_args = instruction_vreg_args(instr->type);
	for (size_t arg_i = 0; arg_i < 3; arg_i++) {
	  if ((vreg_args & (1u << arg_i))) {
	    size_t vreg = instr->args[arg_i];
	    if (vreg_last_uses[vreg] < instruction_location) {
              vreg_last_uses[vreg] = instruction_location;
            }
	  }
	}
      }
    }
  }

  // A value defined before a loop header can be used again after a backedge,
  // even when all of its linear uses appear earlier than the backedge source.
  // Keep those values live through the source block to prevent reuse on the
  // next loop iteration.
  for (size_t block_i = 0; block_i < blocks->count; block_i++) {
    BasicBlock *source_block = &blocks->blocks[block_i];
    if (source_block->count == 0) {
      continue;
    }

    size_t source_location =
      source_block->instructions[source_block->count - 1].id;
    for (size_t edge_i = 0; edge_i < source_block->output_edge_count; edge_i++) {
      BasicBlock *target_block =
        &blocks->blocks[source_block->output_edges[edge_i].basic_block_id];
      if (target_block->count == 0) {
        continue;
      }

      size_t target_location = target_block->instructions[0].id;
      if (target_location > source_location) {
        continue;
      }

      for (size_t vreg_i = 0; vreg_i < blocks->vreg_count; vreg_i++) {
        if (vreg_defined[vreg_i] &&
            vreg_defs[vreg_i] < target_location &&
            vreg_last_uses[vreg_i] < source_location) {
          vreg_last_uses[vreg_i] = source_location;
        }
      }
    }
  }

  // fallback to stack unless we give it a register
  for (size_t vreg_i = 0; vreg_i < blocks->vreg_count; vreg_i++) {
    blocks->vregs[vreg_i].physical_location.on_stack = true;
    blocks->vregs[vreg_i].physical_location.stack_offset = vreg_i;
  }

  // allocate 8 registers (for now)
  // and spill everything else
  // for now codegen will assume all registers are volatile.
  size_t available_registers = 2; // 8;
  bool register_free[] = { true, true, true, true, true, true, true, true };
  instruction_location = 0;

  for (size_t block_i = 0; block_i < blocks->count; block_i++) {
    BasicBlock *block = &blocks->blocks[block_i];
    for (size_t instr_i = 0; instr_i < block->count; instr_i++) {
      instruction_location = block->instructions[instr_i].id;
      /* printf("%zu. ", instruction_location); */
      for (size_t vreg_i = 0; vreg_i < blocks->vreg_count; vreg_i++) {
	if (vreg_defined[vreg_i] &&
            instruction_location == vreg_last_uses[vreg_i] &&
            blocks->vregs[vreg_i].physical_location.on_stack == false) {
	  register_free[blocks->vregs[vreg_i].physical_location.reg - 3] = true;
	}
	if (vreg_defined[vreg_i] &&
            vreg_defs[vreg_i] < vreg_last_uses[vreg_i] &&
            instruction_location == vreg_defs[vreg_i]) {
	  if (blocks->vregs[vreg_i].physical_location.on_stack) {
	    for (size_t i = 0; i < available_registers; i++) {
	      if (register_free[i]) {
		register_free[i] = false;
		blocks->vregs[vreg_i].physical_location.on_stack = false;
		blocks->vregs[vreg_i].physical_location.reg = i + 3;
		i = available_registers;
	      }
	    }
	  }
	}

	/* if (vreg_defined[vreg_i] && */
        /*     instruction_location >= vreg_defs[vreg_i] && */
        /*     instruction_location <= vreg_last_uses[vreg_i]) { */
	/*   printf("V%zu, ", vreg_i); */
	/* } */
      }
      /* printf("\n"); */
    }

  }

  blocks->vreg_defs = vreg_defs;
  blocks->vreg_last_uses = vreg_last_uses;
  free(vreg_defined);
}


void print_vreg_types(MethodBlocks *blocks, size_t vreg) {
  bool first = true;
  printf("(");
  if ((blocks->virtual_register_types[vreg] & JANET_TFLAG_NIL) == JANET_TFLAG_NIL) {
    if (first) {
      first = false;
    } else {
      printf(", ");
    }
    printf("nil");
  } else if ((blocks->virtual_register_types[vreg] & JANET_TFLAG_BOOLEAN) == JANET_TFLAG_BOOLEAN) {
    if (first) {
      first = false;
    } else {
      printf(", ");
    }
    printf("bool");
  } else if ((blocks->virtual_register_types[vreg] & JANET_TFLAG_FIBER) == JANET_TFLAG_FIBER) {
    if (first) {
      first = false;
    } else {
      printf(", ");
    }
    printf("fiber");
  } else if ((blocks->virtual_register_types[vreg] & JANET_TFLAG_NUMBER) == JANET_TFLAG_NUMBER) {
    if (first) {
      first = false;
    } else {
      printf(", ");
    }
    printf("number");
  } else if ((blocks->virtual_register_types[vreg] & JANET_TFLAG_STRING) == JANET_TFLAG_STRING) {
    if (first) {
      first = false;
    } else {
      printf(", ");
    }
    printf("string");
  } else if ((blocks->virtual_register_types[vreg] & JANET_TFLAG_SYMBOL) == JANET_TFLAG_SYMBOL) {
    if (first) {
      first = false;
    } else {
      printf(", ");
    }
    printf("symbol");
  } else if ((blocks->virtual_register_types[vreg] & JANET_TFLAG_KEYWORD) == JANET_TFLAG_KEYWORD) {
    if (first) {
      first = false;
    } else {
      printf(", ");
    }
    printf("keyword");
  } else if ((blocks->virtual_register_types[vreg] & JANET_TFLAG_ARRAY) == JANET_TFLAG_ARRAY) {
    if (first) {
      first = false;
    } else {
      printf(", ");
    }
    printf("array");
  } else if ((blocks->virtual_register_types[vreg] & JANET_TFLAG_TUPLE) == JANET_TFLAG_TUPLE) {
    if (first) {
      first = false;
    } else {
      printf(", ");
    }
    printf("tuple");
  } else if ((blocks->virtual_register_types[vreg] & JANET_TFLAG_TABLE) == JANET_TFLAG_TABLE) {
    if (first) {
      first = false;
    } else {
      printf(", ");
    }
    printf("table");
  } else if ((blocks->virtual_register_types[vreg] & JANET_TFLAG_STRUCT) == JANET_TFLAG_STRUCT) {
    if (first) {
      first = false;
    } else {
      printf(", ");
    }
    printf("struct");
  } else if ((blocks->virtual_register_types[vreg] & JANET_TFLAG_BUFFER) == JANET_TFLAG_BUFFER) {
    if (first) {
      first = false;
    } else {
      printf(", ");
    }
    printf("buffer");
  } else if ((blocks->virtual_register_types[vreg] & JANET_TFLAG_FUNCTION) == JANET_TFLAG_FUNCTION) {
    if (first) {
      first = false;
    } else {
      printf(", ");
    }
    printf("function");
  } else if ((blocks->virtual_register_types[vreg] & JANET_TFLAG_CFUNCTION) == JANET_TFLAG_CFUNCTION) {
    if (first) {
      first = false;
    } else {
      printf(", ");
    }
    printf("cfunction");
  } else if ((blocks->virtual_register_types[vreg] & JANET_TFLAG_ABSTRACT) == JANET_TFLAG_ABSTRACT) {
    if (first) {
      first = false;
    } else {
      printf(", ");
    }
    printf("abstract");
  }
  printf(")");
}

void print_ops(MethodBlocks *blocks, Operand *op) {
  switch (op->type) {
  case OPERAND_VIRTUAL_REGISTER:
    printf("v%zu", op->virtual_register);
    if (op->physical_location.on_stack) {
      printf("[s%zu] ", op->physical_location.stack_offset);
    } else {
      printf("[r%zu] ", op->physical_location.reg);
    }
    print_vreg_types(blocks, op->virtual_register);
    break;
  case OPERAND_BASIC_BLOCK:
    printf("bb%zu", op->bb);
    break;
  case OPERAND_SIGNED_IMM:
    printf("imm%" PRId64, op->imms);
    break;
  case OPERAND_UNSIGNED_IMM:
    printf("imm%" PRIu64, op->immus);
    break;
  case OPERAND_JANET_IMM:
    printf("jimm");
    break;
    case OPERAND_UNUSED:
    printf("_");
    break;
    case OPERAND_UNDEFINED:
    printf("?");
    break;
  }
}

static Operand *instruction_arg(MethodBlocks *blocks,
				Instruction *instr,
				size_t arg_i) {
  InstructionArgs args = instruction_args(instr->type);
  uint8_t arg_mask = 1u << arg_i;

  if (args.vregs & arg_mask) {
    return &blocks->vregs[instr->args[arg_i]];
  }
  if (args.imms & arg_mask) {
    return &blocks->imms[instr->args[arg_i]];
  }
  if (args.bbs & arg_mask) {
    return &blocks->bbs[instr->args[arg_i]];
  }
  return &blocks->vregs[0];
}

void print_basic_blocks(MethodBlocks *blocks) {
  // todo print method name etc.
  for (size_t block_i = 0; block_i < blocks->count; block_i++) {
    BasicBlock *bb = &blocks->blocks[block_i];
    printf("----block %zu : %zu instructions ----\n", block_i, bb->count);
    for (size_t instr_i = 0; instr_i < bb->count; instr_i++) {
      Instruction *instruction = &bb->instructions[instr_i];
      printf("%zu. ", instr_i);
      if (instruction->result != 0) {
	print_ops(blocks, &blocks->vregs[instruction->result]);
	printf(" = ");
      }
      printf("%s [", instruction_names[instruction->type]);
      if (instruction->type == HIR_PHI) {
	print_ops(blocks, &blocks->vregs[instruction->result]);
	for (size_t i = 0; i < instruction->phi_source_count; i++) {
	  if (i > 0) {
	    printf(", ");
	  }
	  printf("(bb%zu: v%zu) ", instruction->phi_sources[i].bb, instruction->phi_sources[i].virtual_register);
	}
      } else {
	for (size_t arg_i = 0; arg_i < 3; arg_i++) {
	  if (arg_i > 0) {
	    printf(", ");
	  }
	  print_ops(blocks, instruction_arg(blocks, instruction, arg_i));
	}
      }
      printf("] \n");


    }
    printf("- - - - - - - - -\n");

    printf("-----------------\n");
  }
}

// c functions called from the jit
uint64_t jit_nil(Janet val) {
  return janet_checktype(val, JANET_NIL);
}

void jit_typecheck(Janet val, uint32_t types) {
  if (!janet_checktypes(val, types)) {
    janet_panicf("expected %T, got %v", types, val);
  }
}

uint64_t jit_binop_helper(Janet lhs, Janet rhs, char *lhs_method, char *rhs_method) {
  Janet method = janet_get(lhs, janet_ckeywordv(lhs_method));
  if (!janet_checktype(method, JANET_NIL)) {
    Janet args[2] = {lhs, rhs};
    return janet_u64(janet_mcall(lhs_method, 2, args));
  } else {
    Janet method = janet_get(rhs, janet_ckeywordv(rhs_method));
    if (!janet_checktype(method, JANET_NIL)) {
      Janet args[2] = {rhs, lhs};
      return janet_u64(janet_mcall(rhs_method, 2, args));
    } else {
      janet_panicf("JIT couldn't find method :%s for %v or %s for %v", lhs_method, lhs, rhs_method, rhs);
    }
  }
}

uint64_t jit_unaryop_helper(Janet arg, char *method_name) {
  Janet method = janet_get(arg, janet_ckeywordv(method_name));
  if (!janet_checktype(method, JANET_NIL)) {
    Janet args[1] = {arg};
    return janet_u64(janet_mcall(method_name, 1, args));
  } else {
    janet_panicf("JIT couldn't find method :%s for %v", method, arg);
  }
}

// This path can't prove numeric, but still might be numeric
uint64_t jit_add_fallback(Janet lhs, Janet rhs) {
  if (janet_checktype(lhs, JANET_NUMBER) && janet_checktype(rhs, JANET_NUMBER)) {
    return janet_u64(janet_wrap_number(janet_unwrap_number(lhs) + janet_unwrap_number(rhs)));
  }
  return jit_binop_helper(lhs, rhs, "+", "r+");
}

uint64_t jit_sub_fallback(Janet lhs, Janet rhs) {
  if (janet_checktype(lhs, JANET_NUMBER) && janet_checktype(rhs, JANET_NUMBER)) {
    return janet_u64(janet_wrap_number(janet_unwrap_number(lhs) - janet_unwrap_number(rhs)));
  }
  return jit_binop_helper(lhs, rhs, "-", "r-");
}

uint64_t jit_mul_fallback(Janet lhs, Janet rhs) {
  if (janet_checktype(lhs, JANET_NUMBER) && janet_checktype(rhs, JANET_NUMBER)) {
    return janet_u64(janet_wrap_number(janet_unwrap_number(lhs) * janet_unwrap_number(rhs)));
  }
  return jit_binop_helper(lhs, rhs, "*", "r*");
}

uint64_t jit_div_fallback(Janet lhs, Janet rhs) {
  if (janet_checktype(lhs, JANET_NUMBER) && janet_checktype(rhs, JANET_NUMBER)) {
    return janet_u64(janet_wrap_number(janet_unwrap_number(lhs) / janet_unwrap_number(rhs)));
  }
  return jit_binop_helper(lhs, rhs, "/", "r/");
}

// TODO: does divf use a different method when calling a struct/table?
uint64_t jit_divf_fallback(Janet lhs, Janet rhs) {
  if (janet_checktype(lhs, JANET_NUMBER) && janet_checktype(rhs, JANET_NUMBER)) {
    return janet_u64(janet_wrap_number(floor(janet_unwrap_number(lhs) / janet_unwrap_number(rhs))));
  }
  return jit_binop_helper(lhs, rhs, "div", "rdiv");
}

uint64_t jit_rem_fallback(Janet lhs, Janet rhs) {
  if (janet_checktype(lhs, JANET_NUMBER) && janet_checktype(rhs, JANET_NUMBER)) {
    return janet_u64(janet_wrap_number(fmod(janet_unwrap_number(lhs), janet_unwrap_number(rhs))));
  }
  return jit_binop_helper(lhs, rhs, "%", "r%");
}

uint64_t jit_mod_fallback(Janet lhs, Janet rhs) {
  if (janet_checktype(lhs, JANET_NUMBER) && janet_checktype(rhs, JANET_NUMBER)) {
    double x1 = janet_unwrap_number(lhs);
    double x2 = janet_unwrap_number(rhs);
    if (x2 == 0) {
      return janet_u64(janet_wrap_number(x1));
    } else {
      double intres = x2 * floor(x1 / x2);
      return janet_u64(janet_wrap_number(x1 - intres));
    }
  }
  return jit_binop_helper(lhs, rhs, "mod", "rmod");
}

uint64_t jit_band_fallback(Janet lhs, Janet rhs) {
  if (janet_checktype(lhs, JANET_NUMBER) && janet_checktype(rhs, JANET_NUMBER)) {
    return janet_u64(janet_wrap_integer(janet_unwrap_integer(lhs) & janet_unwrap_integer(rhs)));
  }
  return jit_binop_helper(lhs, rhs, "&", "r&");
}

uint64_t jit_bor_fallback(Janet lhs, Janet rhs) {
  if (janet_checktype(lhs, JANET_NUMBER) && janet_checktype(rhs, JANET_NUMBER)) {
    return janet_u64(janet_wrap_integer(janet_unwrap_integer(lhs) | janet_unwrap_integer(rhs)));
  }
  return jit_binop_helper(lhs, rhs, "|", "r|");
}

uint64_t jit_bxor_fallback(Janet lhs, Janet rhs) {
  if (janet_checktype(lhs, JANET_NUMBER) && janet_checktype(rhs, JANET_NUMBER)) {
    return janet_u64(janet_wrap_integer(janet_unwrap_integer(lhs) ^ janet_unwrap_integer(rhs)));
  }
  return jit_binop_helper(lhs, rhs, "^", "r^");
}

uint64_t jit_bnot_fallback(Janet arg) {
  if (janet_checktype(arg, JANET_NUMBER)) {
    return janet_u64(janet_wrap_integer(~janet_unwrap_integer(arg)));
  }
  return jit_unaryop_helper(arg, "~");
}

// TODO: these need range checks
uint64_t jit_blshift_fallback(Janet lhs, Janet rhs) {
  if (janet_checktype(lhs, JANET_NUMBER) && janet_checktype(rhs, JANET_NUMBER)) {
    return janet_u64(janet_wrap_integer(janet_unwrap_integer(lhs) << janet_unwrap_integer(rhs)));
  }
  return jit_binop_helper(lhs, rhs, "<<", "r<<");
}

uint64_t jit_brshift_fallback(Janet lhs, Janet rhs) {
  if (janet_checktype(lhs, JANET_NUMBER) && janet_checktype(rhs, JANET_NUMBER)) {
    return janet_u64(janet_wrap_integer(janet_unwrap_integer(lhs) >> janet_unwrap_integer(rhs)));
  }
  return jit_binop_helper(lhs, rhs, ">>", "r>>");
}

uint64_t jit_brushift_fallback(Janet lhs, Janet rhs) {
  if (janet_checktype(lhs, JANET_NUMBER) && janet_checktype(rhs, JANET_NUMBER)) {
    return janet_u64(janet_wrap_number((uint32_t)(janet_unwrap_integer(lhs) >> janet_unwrap_integer(rhs))));
  }
  return jit_binop_helper(lhs, rhs, ">>", "r>>");
}

uint64_t jit_greater_than_fallback(Janet lhs, Janet rhs) {
  return janet_u64(janet_wrap_boolean(janet_compare(lhs, rhs) > 0));
}

uint64_t jit_less_than_fallback(Janet lhs, Janet rhs) {
  return janet_u64(janet_wrap_boolean(janet_compare(lhs, rhs) < 0));
}

uint64_t jit_greater_than_equal_fallback(Janet lhs, Janet rhs) {
  return janet_u64(janet_wrap_boolean(janet_compare(lhs, rhs) >= 0));
}

uint64_t jit_less_than_equal_fallback(Janet lhs, Janet rhs) {
  return janet_u64(janet_wrap_boolean(janet_compare(lhs, rhs) <= 0));
}

uint64_t jit_equals_fallback(Janet lhs, Janet rhs) {
  return janet_u64(janet_wrap_boolean(janet_equals(lhs, rhs)));
}

uint64_t jit_not_equals_fallback(Janet lhs, Janet rhs) {
  return janet_u64(janet_wrap_boolean(!janet_equals(lhs, rhs)));
}

uint64_t jit_compare_fallback(Janet lhs, Janet rhs) {
  return janet_u64(janet_wrap_number(janet_compare(lhs, rhs)));
}



typedef uint64_t (*JitBinaryFallback)(Janet lhs, Janet rhs);
typedef uint64_t (*JitUnaryFallback)(Janet val);

void ensure_argv_space(CallArgs *call_args, size_t request) {
  if (call_args->capacity == 0) {
    call_args->capacity = (request > 8) ? request : 8;
    call_args->argv = malloc(call_args->capacity * sizeof(Janet));
  } else if (call_args->count + request > call_args->capacity) {
    call_args->capacity *= 2;
    call_args->argv = realloc(call_args->argv, call_args->capacity * sizeof(Janet));
  }
}

void jit_push(CallArgs *call_args, Janet value) {
  ensure_argv_space(call_args, 1);
  call_args->argv[call_args->count++] = value;
}

void jit_push_2(CallArgs *call_args, Janet value1, Janet value2) {
  ensure_argv_space(call_args, 2);
  call_args->argv[call_args->count++] = value1;
  call_args->argv[call_args->count++] = value2;
}

void jit_push_3(CallArgs *call_args, Janet value1, Janet value2, Janet value3) {
  ensure_argv_space(call_args, 3);
  call_args->argv[call_args->count++] = value1;
  call_args->argv[call_args->count++] = value2;
  call_args->argv[call_args->count++] = value3;
}

uint64_t jit_call(Janet callee, CallArgs *call_args) {
  // Treat the CallArgs buffer like Janet's fiber arg stack: argv_offset is the
  // current frame's stackstart, count is stacktop. Save the base, isolate the
  // nested frame so re-entrant pushes land above our args, dispatch every
  // callee kind with the same argc/argv, then restore the base and pop.
  size_t prev_offset = call_args->argv_offset;
  Janet *argv = call_args->argv + prev_offset;
  int32_t argc = (int32_t)(call_args->count - prev_offset);
  call_args->argv_offset = call_args->count;

  uint64_t result;
  if (janet_checktype(callee, JANET_FUNCTION)) {
    result = janet_u64(janet_call(janet_unwrap_function(callee), argc, argv));
  } else if (janet_checktype(callee, JANET_CFUNCTION)) {
    int gc_lock = janet_gclock();
    result = janet_u64(janet_unwrap_cfunction(callee)(argc, argv));
    janet_gcunlock(gc_lock);
  } else if (janet_checktype(callee, JANET_ABSTRACT)) {
    JanetAbstract abstract = janet_unwrap_abstract(callee);
    const JanetAbstractType *at = janet_abstract_type(abstract);
    if (at->call != NULL) {
      int gc_lock = janet_gclock();
      result = janet_u64(at->call(abstract, argc, argv));
      janet_gcunlock(gc_lock);
    } else {
      janet_panic("attempted to call uncallable abstract type");
    }
  } else if (janet_checktype(callee, JANET_KEYWORD)) {
    if (argc == 0) {
      janet_panic("keyword argument on nil value");
    }

    Janet kwcallee = janet_get(argv[0], callee);
    if (janet_checktype(kwcallee, JANET_FUNCTION)) {
      result = janet_u64(janet_call(janet_unwrap_function(kwcallee), argc, argv));
    } else {
      janet_panicf("keyword function %p, %p is not callable", callee, kwcallee);
    }
  } else {
    janet_panic("attempted to call uncallable type");
  }

  call_args->argv_offset = prev_offset;
  call_args->count = prev_offset;
  return result;
}

uint64_t jit_make_array(CallArgs *call_args) {
  Janet a = janet_wrap_array(janet_array_n(call_args->argv + call_args->argv_offset, call_args->count - call_args->argv_offset));
  call_args->count = call_args->argv_offset;
  return janet_u64(a);
}

uint64_t jit_make_tuple(CallArgs *call_args) {
  Janet tup = janet_wrap_tuple(janet_tuple_n(call_args->argv + call_args->argv_offset, call_args->count - call_args->argv_offset));
  call_args->count = call_args->argv_offset;
  return janet_u64(tup);
}

uint64_t jit_make_bracket_tuple(CallArgs *call_args) {
  JanetTuple t = janet_tuple_n(call_args->argv + call_args->argv_offset, call_args->count - call_args->argv_offset);
  janet_tuple_flag(t) |= JANET_TUPLE_FLAG_BRACKETCTOR;
  Janet tup = janet_wrap_tuple(t);
  call_args->count = call_args->argv_offset;
  return janet_u64(tup);
}

uint64_t jit_make_buffer(CallArgs *call_args) {
  JanetBuffer *b = janet_buffer((call_args->count - call_args->argv_offset) * 10);
  for (size_t i = 0; i < (call_args->count - call_args->argv_offset); i++) {
    janet_to_string_b(b, call_args->argv[i + call_args->argv_offset]);
  }
  call_args->count = call_args->argv_offset;
  return janet_u64(janet_wrap_buffer(b));
}

uint64_t jit_make_string(CallArgs *call_args) {
  JanetBuffer *b = janet_buffer((call_args->count - call_args->argv_offset) * 10);
  for (size_t i = 0; i < (call_args->count - call_args->argv_offset); i++) {
    janet_to_string_b(b, call_args->argv[i + call_args->argv_offset]);
  }
  call_args->count = call_args->argv_offset;
  // TODO: this leaves a garbage buffer we can potentially skip
  return janet_u64(janet_stringv(b->data, b->count));
}

uint64_t jit_make_table(CallArgs *call_args) {
  if ((call_args->count - call_args->argv_offset) & 1) {
    janet_panicf("expected even number of arguments to table constructor, got %d", (call_args->count - call_args->argv_offset));
  }
  JanetTable *tab = janet_table((call_args->count - call_args->argv_offset) / 2);
  for (size_t i = 0; i < (call_args->count - call_args->argv_offset); i += 2) {
    janet_table_put(tab, call_args->argv[i + call_args->argv_offset], call_args->argv[i + 1 + call_args->argv_offset]);
  }
  call_args->count = call_args->argv_offset;
  return janet_u64(janet_wrap_table(tab));
}

uint64_t jit_make_struct(CallArgs *call_args) {
  if ((call_args->count - call_args->argv_offset) & 1) {
    janet_panicf("expected even number of arguments to struct constructor, got %d", (call_args->count - call_args->argv_offset));
  }
  JanetKV *st = janet_struct_begin((call_args->count - call_args->argv_offset) / 2);
  for (size_t i = 0; i < (call_args->count - call_args->argv_offset); i += 2) {
    janet_struct_put(st, call_args->argv[i + call_args->argv_offset], call_args->argv[i + 1 + call_args->argv_offset]);
  }
  call_args->count = call_args->argv_offset;
  return janet_u64(janet_wrap_struct(janet_struct_end(st)));
}

typedef Janet (*JitFn)(Janet *argv, CallArgs *call_args);

// codegen helpers
bool operand_numeric(uint32_t *types, Operand *op1) {
  return types[op1->virtual_register] == JANET_TFLAG_NUMBER;
}

bool operands_numeric(uint32_t *types, Operand *op1, Operand *op2) {
  return types[op1->virtual_register] == JANET_TFLAG_NUMBER &&
    (op2->type == OPERAND_SIGNED_IMM || op2->type == OPERAND_UNSIGNED_IMM || types[op2->virtual_register] == JANET_TFLAG_NUMBER);
}

// codegen
static void emit_byte(CodeBuffer *code, uint8_t byte) {
  if (code->count >= code->capacity) {
    if (code->count < SIZE_MAX / 2) {
      code->capacity *= 2;
      code->data = realloc(code->data, code->capacity * sizeof(uint8_t));
      if (code->data == NULL) {
	janet_panic("JIT could not allocate while generating bytecode");
      }
    } else {
      janet_panic("JIT ran out of memory generating bytecode");
    }
  }
  code->data[code->count++] = byte;
}

static void emit_u32(CodeBuffer *code, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    emit_byte(code, (uint8_t)(value >> shift));
  }
}

static void edit_u32(CodeBuffer *code, uint32_t index, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    code->data[index + shift / 8] = (uint8_t)(value >> shift);
  }
}

static void emit_u64(CodeBuffer *code, uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    emit_byte(code, (uint8_t)(value >> shift));
  }
}

void emit_arg_to_xmm(CodeBuffer *code, uint32_t arg_num, uint32_t xmm_num) {
  emit_byte(code, 0xF2);
  emit_byte(code, 0x0F);
  emit_byte(code, 0x10);
  emit_byte(code, 0x87 | ((xmm_num & 7) << 3));
  emit_u32(code, arg_num * sizeof(Janet));
}

void emit_arg_to_stack(CodeBuffer *code, uint32_t arg_num, uint32_t stack_num) {
  emit_byte(code, 0x48);
  emit_byte(code, 0x8B); // MOV rax, [rdi + offset]
  emit_byte(code, 0x87);
  emit_u32(code, arg_num * sizeof(Janet));
  // RAX -> stack + offset
  emit_byte(code, 0x48);
  emit_byte(code, 0x89);
  emit_byte(code, 0x84);
  emit_byte(code, 0x24);
  emit_u32(code, stack_num * sizeof(Janet));
}

static void emit_stack_to_xmm(CodeBuffer *code, uint32_t dest, uint32_t source) {
  // xmm(dest), rdi + source
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

static void emit_stack_to_rax(CodeBuffer *code, uint32_t source) {
  // mov rax, rsp + offset
  emit_byte(code, 0x48);
  emit_byte(code, 0x8B);
  emit_byte(code, 0x84);
  emit_byte(code, 0x24);
  emit_u32(code, source * sizeof(Janet));
}

static void emit_stack_to_gpr_bits(CodeBuffer *code,
                                   uint32_t dest,
                                   uint32_t source) {
  emit_byte(code, 0x48 | ((dest & 8) ? 0x04 : 0));
  emit_byte(code, 0x8B);
  emit_byte(code, 0x84 + ((dest & 7) << 3));
  emit_byte(code, 0x24);
  emit_u32(code, source * sizeof(Janet));
}

static void emit_stack_to_gpr(CodeBuffer *code, uint32_t dest, uint32_t source) {
  emit_byte(code, 0xF2);
  emit_byte(code, 0x48 | ((dest & 8) ? 0x04 : 0));
  emit_byte(code, 0x0F);
  emit_byte(code, 0x2C);
  emit_byte(code, 0x84 + ((dest & 7) << 3)); // store here
  emit_byte(code, 0x24);
  emit_u32(code, source * sizeof(Janet)); // from here
}

static void emit_xmm_to_integer_gpr(CodeBuffer *code,
                                    uint32_t destination,
                                    uint32_t source) {
  emit_byte(code, 0xF2);
  emit_byte(code, 0x48 |
            ((destination & 8) ? 0x04 : 0) |
            ((source & 8) ? 0x01 : 0));
  emit_byte(code, 0x0F);
  emit_byte(code, 0x2C);
  emit_byte(code, 0xC0 |
            ((destination & 7) << 3) |
            (source & 7));
}


static void emit_imm_gpr(CodeBuffer *code, uint32_t destination, uint64_t val) {
  emit_byte(code, 0x48 | ((destination & 8) ? 0x01 : 0));
  emit_byte(code, 0xB8 | (destination & 7));
  emit_u64(code, val);
}

static void emit_gpr_to_xmm(CodeBuffer *code, uint32_t destination, uint32_t source) {
  emit_byte(code, 0x66);
  emit_byte(code, 0x48 |
	    ((destination & 8) ? 0x04 : 0) |
	    ((source & 8) ? 0x01 : 0));
  emit_byte(code, 0x0F);
  emit_byte(code, 0x6E);
  emit_byte(code, 0xC0 |
	    ((destination & 7) << 3) |
	    (source & 7));
}

static void emit_xmm_to_gpr(CodeBuffer *code, uint32_t destination, uint32_t source) {
  emit_byte(code, 0x66);
  emit_byte(code, 0x48 |
	    ((source & 8) ? 0x04 : 0) |
	    ((destination & 8) ? 0x01 : 0));
  emit_byte(code, 0x0F);
  emit_byte(code, 0x7E);
  emit_byte(code, 0xC0 |
	    ((source & 7) << 3) |
	    (destination & 7));
}

static uint64_t operand_to_janet_bits(Operand *operand) {
  switch (operand->type) {
  case OPERAND_SIGNED_IMM:
    return janet_u64(janet_wrap_number((double) operand->imms));
  case OPERAND_UNSIGNED_IMM:
    return janet_u64(janet_wrap_number((double) operand->immus));
  case OPERAND_JANET_IMM:
    return operand->immus;
  default:
    janet_panic("cannot materialize operand as a Janet value");
  }
}

static void emit_xmm_mov(CodeBuffer *code, uint32_t dest, uint32_t source);

static void emit_op_to_xmm(CodeBuffer *code, uint32_t destination, Operand *operand) {
  if (operand->type == OPERAND_VIRTUAL_REGISTER) {
    if (operand->physical_location.on_stack) {
      emit_stack_to_xmm(code, destination, operand->physical_location.stack_offset);
    } else {
      emit_xmm_mov(code, destination, operand->physical_location.reg);
    }
    return;
  }
  emit_imm_gpr(code, 0, operand_to_janet_bits(operand));
  emit_gpr_to_xmm(code, destination, 0);
}

static void emit_op_to_gpr(CodeBuffer *code, uint32_t destination, Operand *operand) {
  if (operand->type == OPERAND_VIRTUAL_REGISTER) {
    if (operand->physical_location.on_stack) {
      emit_stack_to_gpr_bits(code, destination, operand->physical_location.stack_offset);
    } else {
      emit_xmm_to_gpr(code, destination, operand->physical_location.reg);
    }
    return;
  }
  emit_imm_gpr(code, destination, operand_to_janet_bits(operand));
}

static void emit_op_to_integer_gpr(CodeBuffer *code,
                                   uint32_t destination,
                                   Operand *operand) {
  if (operand->type == OPERAND_VIRTUAL_REGISTER) {
    if (operand->physical_location.on_stack) {
      emit_stack_to_gpr(code, destination, operand->physical_location.stack_offset);
    } else {
      emit_xmm_to_integer_gpr(code, destination, operand->physical_location.reg);
    }
    return;
  }

  switch (operand->type) {
  case OPERAND_SIGNED_IMM:
    emit_imm_gpr(code, destination, (uint64_t)operand->imms);
    return;
  case OPERAND_UNSIGNED_IMM:
    emit_imm_gpr(code, destination, operand->immus);
    return;
  default:
    janet_panic("cannot materialize operand as an integer");
  }
}

static void emit_imm_to_xmm(CodeBuffer *code, uint32_t destination, Operand *operand) {
  emit_imm_gpr(code, 0, operand_to_janet_bits(operand));
  emit_gpr_to_xmm(code, destination, 0);
}

static void emit_store_rax(CodeBuffer *code, uint32_t offset) {
  emit_byte(code, 0x48);
  emit_byte(code, 0x89);
  emit_byte(code, 0x84);
  emit_byte(code, 0x24);
  emit_u32(code, offset * sizeof(Janet));
}

static void emit_xmm_mov(CodeBuffer *code, uint32_t dest, uint32_t source) {
  emit_byte(code, 0xF2);
  emit_byte(code, 0x0F);
  emit_byte(code, 0x10);
  emit_byte(code, 0xC0 | ((dest & 7) << 3) | (source & 7));
}

static void emit_gpr_to_stack_bits(CodeBuffer *code,
                                   uint32_t destination,
                                   uint32_t source) {
  emit_byte(code, 0x48 | ((source & 8) ? 0x01 : 0));
  emit_byte(code, 0x89);
  emit_byte(code, 0x84 + ((source & 7) << 3));
  emit_byte(code, 0x24);
  emit_u32(code, destination * sizeof(Janet));
}

static void emit_gpr_to_op(CodeBuffer *code, Operand *dest, uint32_t source) {
  if (dest->physical_location.on_stack) {
    emit_gpr_to_stack_bits(code, dest->physical_location.stack_offset, source);
  } else {
    emit_gpr_to_xmm(code, dest->physical_location.reg, source);
  }
}

static void emit_rax_to_op(CodeBuffer *code, Operand *dest) {
  emit_gpr_to_op(code, dest, 0);
}

static void emit_xmm_to_op(CodeBuffer *code, Operand *dest, uint32_t source) {
  if (dest->physical_location.on_stack) {
    emit_xmm_to_stack(code, dest->physical_location.stack_offset, source);
  } else {
    emit_xmm_mov(code, dest->physical_location.reg, source);
  }
}

static void emit_gpr_to_number_op(CodeBuffer *code, Operand *dest, uint32_t source) {
  emit_byte(code, 0xF2);
  emit_byte(code, 0x48 | ((source & 8) ? 0x01 : 0));
  emit_byte(code, 0x0F);
  emit_byte(code, 0x2A);
  emit_byte(code, 0xC0 | (source & 7));
  emit_xmm_to_op(code, dest, 0);
}

static void emit_store_imm_op(CodeBuffer *code, Operand *op, Operand *imm) {
  emit_imm_gpr(code, 0, operand_to_janet_bits(imm));
  emit_rax_to_op(code, op);
}

#define X86_ADD 0x58
#define X86_MUL 0x59
#define X86_SUB 0x5C
#define X86_DIV 0x5E

static void emit_binary_op_op(CodeBuffer *code, uint8_t op, Operand *dest, Operand *lhs, Operand *rhs) {
  // TODO: if we know lhs is a register and the same we can reuse it here.
  emit_op_to_xmm(code, 0, lhs);
  emit_op_to_xmm(code, 1, rhs);

  emit_byte(code, 0xF2);
  emit_byte(code, 0x0F);
  emit_byte(code, op);
  emit_byte(code, 0xC0 + (0 << 3) + 1);

  emit_xmm_to_op(code, dest, 0);
}

// TODO: make this unused
static void emit_binary_op(CodeBuffer *code, uint8_t op, uint32_t dest, uint32_t lhs, uint32_t rhs) {
  if (dest != lhs) {
    emit_xmm_mov(code, dest, lhs);
  }

  emit_byte(code, 0xF2);
  emit_byte(code, 0x0F);
  emit_byte(code, op);
  emit_byte(code, 0xC0 + (dest << 3) + rhs);
}

static void emit_ucomisd(CodeBuffer *code, uint32_t lhs, uint32_t rhs) {
  emit_byte(code, 0x66);
  if ((lhs | rhs) & 8) {
    emit_byte(code, 0x40 |
	      ((lhs & 8) ? 0x04 : 0) |
	      ((rhs & 8) ? 0x01 : 0));
  }
  emit_byte(code, 0x0F);
  emit_byte(code, 0x2E);
  emit_byte(code, 0xC0 |
	    ((lhs & 7) << 3) |
	    (rhs & 7));
}

static void emit_cmov(CodeBuffer *code, uint8_t condition, uint32_t destination, uint32_t source) {
  emit_byte(code, 0x48 |
	    ((destination & 8) ? 0x04 : 0) |
	    ((source & 8) ? 0x01 : 0));
  emit_byte(code, 0x0F);
  emit_byte(code, condition);
  emit_byte(code, 0xC0 |
	    ((destination & 7) << 3) |
	    (source & 7));
}

#define X86_CMOV_BELOW 0x42
#define X86_CMOV_ABOVE_EQUAL 0x43
#define X86_CMOV_EQUAL 0x44
#define X86_CMOV_NOT_EQUAL 0x45
#define X86_CMOV_ABOVE 0x47
#define X86_CMOV_PARITY 0x4A

static void emit_comparison(CodeBuffer *code,
			    Operand *lhs,
			    Operand *rhs,
			    bool reverse,
			    uint8_t condition,
			    int unordered_result,
			    Operand *destination) {
  emit_op_to_xmm(code, reverse ? 1 : 0, lhs);
  emit_op_to_xmm(code, reverse ? 0 : 1, rhs);
  emit_ucomisd(code, 0, 1);

  // MOV and CMOV preserve the comparison flags set by UCOMISD.
  emit_imm_gpr(code, 0, janet_u64(janet_wrap_false()));
  emit_imm_gpr(code, 1, janet_u64(janet_wrap_true()));
  emit_cmov(code, condition, 0, 1);

  if (unordered_result >= 0) {
    emit_imm_gpr(code,
		 2,
		 janet_u64(unordered_result
			   ? janet_wrap_true()
			   : janet_wrap_false()));
    emit_cmov(code, X86_CMOV_PARITY, 0, 2);
  }

  emit_rax_to_op(code, destination);
}

static void emit_floor(CodeBuffer *code, uint32_t reg) {
  emit_byte(code, 0x66);
  emit_byte(code, 0x0F);
  emit_byte(code, 0x3A);
  emit_byte(code, 0x0B);
  emit_byte(code, 0xC0 + (reg << 3) + reg);
  emit_byte(code, 0x01);
}

static void emit_trunc(CodeBuffer *code, uint32_t reg) {
  emit_byte(code, 0x66);
  emit_byte(code, 0x0F);
  emit_byte(code, 0x3A);
  emit_byte(code, 0x0B);
  emit_byte(code, 0xC0 + (reg << 3) + reg);
  emit_byte(code, 0x03);
}

#define X86_OR_GPR 0x09
#define X86_AND_GPR 0x21
#define X86_XOR_GPR 0x31

static void emit_gpr_op(CodeBuffer *code, uint8_t op, uint32_t lhs, uint32_t rhs) {
  emit_byte(code, 0x48 |
	    ((rhs & 8) ? 0x04 : 0) |
	    ((lhs & 8) ? 0x01 : 0));
  emit_byte(code, op);
  emit_byte(code, 0xC0 | ((rhs & 7) << 3) | (lhs & 7));
}

static void emit_not(CodeBuffer *code, uint32_t val) {
  emit_byte(code, 0x48 | ((val & 8) ? 0x01 : 0));
  emit_byte(code, 0xF7);
  emit_byte(code, 0xD0 | (val & 7));
}

static void emit_gpr_to_stack(CodeBuffer *code, uint32_t destination, uint32_t source) {
  // Convert the integer in gpr back to a Janet number in XMM0.
  emit_byte(code, 0xF2);
  emit_byte(code, 0x48 | ((source & 8) ? 0x01 : 0));
  emit_byte(code, 0x0F);
  emit_byte(code, 0x2A);
  emit_byte(code, 0xC0 | (source & 7));
  emit_xmm_to_stack(code, destination, 0);
}

#define X86_SHIFT_LEFT 4
#define X86_SHIFT_RIGHT_UNSIGNED 5
#define X86_SHIFT_RIGHT_SIGNED 7

static void emit_gpr_shift(CodeBuffer *code, uint8_t operation, uint32_t destination) {
  if (destination & 8) {
    emit_byte(code, 0x41);
  }
  emit_byte(code, 0xD3);
  emit_byte(code, 0xC0 |
	    ((operation & 7) << 3) |
	    (destination & 7));
}

static void emit_gpr_shift_immediate(CodeBuffer *code, uint8_t operation, uint32_t destination, uint8_t amount) {
  if (destination & 8) {
    emit_byte(code, 0x41);
  }
  emit_byte(code, 0xC1);
  emit_byte(code, 0xC0 |
	    ((operation & 7) << 3) |
	    (destination & 7));
  emit_byte(code, amount);
}

static void emit_sign_extend_gpr_32(CodeBuffer *code, uint32_t reg) {
  emit_byte(code, 0x48 |
	    ((reg & 8) ? 0x05 : 0));
  emit_byte(code, 0x63);
  emit_byte(code, 0xC0 |
	    ((reg & 7) << 3) |
	    (reg & 7));
}

static void emit_shift(CodeBuffer *code,
		       uint8_t operation,
		       bool signed_result,
		       Operand *destination,
		       Operand *lhs,
		       Operand *rhs) {
  emit_op_to_integer_gpr(code, 0, lhs);

  if (rhs->type == OPERAND_VIRTUAL_REGISTER) {
    emit_op_to_integer_gpr(code, 1, rhs);
    emit_gpr_shift(code, operation, 0);
  } else if (rhs->type == OPERAND_SIGNED_IMM) {
    emit_gpr_shift_immediate(code, operation, 0, (uint8_t) rhs->imms);
  } else if (rhs->type == OPERAND_UNSIGNED_IMM) {
    emit_gpr_shift_immediate(code, operation, 0, (uint8_t) rhs->immus);
  } else {
    janet_panic("unsupported shift operand");
  }

  if (signed_result) {
    emit_sign_extend_gpr_32(code, 0);
  }
  emit_gpr_to_number_op(code, destination, 0);
}

static void emit_compare(CodeBuffer *code,
			 Operand *lhs,
			 Operand *rhs,
			 Operand *destination) {
  emit_op_to_xmm(code, 0, lhs);
  emit_op_to_xmm(code, 1, rhs);
  emit_ucomisd(code, 0, 1);

  // MOV and CMOV preserve the comparison flags set by UCOMISD.
  emit_imm_gpr(code, 0, janet_u64(janet_wrap_integer(0)));
  emit_imm_gpr(code, 1, janet_u64(janet_wrap_integer(-1)));
  emit_imm_gpr(code, 2, janet_u64(janet_wrap_integer(1)));
  emit_cmov(code, X86_CMOV_BELOW, 0, 1);
  emit_cmov(code, X86_CMOV_ABOVE, 0, 2);
  emit_cmov(code, X86_CMOV_PARITY, 0, 2);

  emit_rax_to_op(code, destination);
}

// used for first arg when jumping back to C
static int arg_loc[] = { 7, 6, 2, 1 };

static void emit_op_to_arg(CodeBuffer *code, uint32_t dest, Operand *source) {
  emit_op_to_gpr(code, arg_loc[dest], source);
}

static void emit_stack_to_arg(CodeBuffer *code, uint32_t dest, uint32_t stack_loc) {
  emit_byte(code, 0x48);
  emit_byte(code, 0x8B);
  emit_byte(code, 0x84 + (arg_loc[dest] << 3));
  emit_byte(code, 0x24);
  emit_u32(code, stack_loc * sizeof(Janet));
}

static void emit_imm_to_arg(CodeBuffer *code, uint32_t dest, uint64_t imm) {
  emit_byte(code, 0x48);
  emit_byte(code, 0xB8 + arg_loc[dest]);
  emit_u64(code, imm);
}

static void emit_non_janet_to_arg(CodeBuffer *code, uint32_t dest, uint32_t stack_loc) {
  emit_byte(code, 0x48);
  emit_byte(code, 0x8B);
  emit_byte(code, 0x84 + (arg_loc[dest] << 3));
  emit_byte(code, 0x24);
  emit_u32(code, stack_loc);
}

static void emit_spill_vreg(CodeBuffer *code, Operand *op) {
  emit_xmm_to_stack(code, op->physical_location.stack_offset, op->physical_location.reg);
}

static void emit_restore_vreg(CodeBuffer *code, Operand *op) {
  emit_stack_to_xmm(code, op->physical_location.reg, op->physical_location.stack_offset);
}


static void emit_cfun_call(CodeBuffer *code, MethodBlocks *blocks, Instruction *instr, void *func) {
  // spill registers
  for (size_t vreg_i = 0; vreg_i < blocks->vreg_count; vreg_i++) {
    if (blocks->vreg_defs[vreg_i] < instr->id && instr->id < blocks->vreg_last_uses[vreg_i] &&
	blocks->vregs[vreg_i].physical_location.on_stack == false) {
      emit_spill_vreg(code, &blocks->vregs[vreg_i]);
    }
  }

  emit_byte(code, 0x48);
  emit_byte(code, 0xB8);
  emit_u64(code, (uint64_t)(uintptr_t)func);
  emit_byte(code, 0xFF);
  emit_byte(code, 0xD0);

  // restore registers
  for (size_t vreg_i = 0; vreg_i < blocks->vreg_count; vreg_i++) {
    if (blocks->vreg_defs[vreg_i] < instr->id && instr->id < blocks->vreg_last_uses[vreg_i] &&
	blocks->vregs[vreg_i].physical_location.on_stack == false) {
      emit_restore_vreg(code, &blocks->vregs[vreg_i]);
    }
  }
}

static void emit_arg_to_frame(CodeBuffer *code, uint32_t source, uint32_t stack_loc) {
  emit_byte(code, 0x48);
  emit_byte(code, 0x89);
  emit_byte(code, 0x84 + (arg_loc[source] << 3));
  emit_byte(code, 0x24);
  emit_u32(code, stack_loc);
}

// rax -> stack rsp + stack_offset
static void emit_store_ret(CodeBuffer *code, uint32_t offset) {
  emit_byte(code, 0x48);
  emit_byte(code, 0x89);
  emit_byte(code, 0x84);
  emit_byte(code, 0x24);
  emit_u32(code, offset * sizeof(Janet));
}

static void emit_mov(CodeBuffer *code, Operand *dest, Operand *source) {
  emit_op_to_gpr(code, 0, source);
  emit_rax_to_op(code, dest);
}

static void emit_phi_moves(CodeBuffer *code, MethodBlocks *blocks, uint32_t bb_source, uint32_t bb_target) {
  BasicBlock *target_block = &blocks->blocks[bb_target];

  // Snapshot register-resident sources first so parallel PHI copies cannot
  // overwrite a source needed by a later copy.
  for (size_t pc = 0; pc < target_block->count; pc++) {
    Instruction *instruction = &target_block->instructions[pc];
    if (instruction->type != HIR_PHI) {
      continue;
    }

    for (size_t phi_i = 0; phi_i < instruction->phi_source_count; phi_i++) {
      PhiSource *phi_source = &instruction->phi_sources[phi_i];
      if (phi_source->bb != bb_source) {
        continue;
      }

      Operand *source = &blocks->vregs[phi_source->virtual_register];
      if (!source->physical_location.on_stack) {
        emit_op_to_gpr(code, 0, source);
        emit_gpr_to_stack_bits(code, source->virtual_register, 0);
      }
    }
  }

  for (size_t pc = 0; pc < target_block->count; pc++) {
    Instruction *instruction = &target_block->instructions[pc];
    if (instruction->type != HIR_PHI) {
      continue;
    }

    for (size_t phi_i = 0; phi_i < instruction->phi_source_count; phi_i++) {
      PhiSource *phi_source = &instruction->phi_sources[phi_i];
      if (phi_source->bb == bb_source) {
        Operand source = blocks->vregs[phi_source->virtual_register];
        source.physical_location = (PhysicalLocation) {
          .on_stack = true,
          .stack_offset = source.virtual_register,
        };
        emit_mov(code, &blocks->vregs[instruction->result], &source);
      }
    }
  }
}

static void emit_jump_placeholder(CodeBuffer *code, int32_t pc, size_t target_bb) {
  if (code->jump_index >= code->jump_capacity) {
    if (code->jump_capacity > SIZE_MAX / 2 ||
	code->jump_capacity * 2 > SIZE_MAX / sizeof(size_t)) {
      janet_panic("JIT ran out of memory generating jump placeholders");
    }

    size_t new_capacity = code->jump_capacity * 2;
    size_t *new_targets =
      realloc(code->jump_targets, new_capacity * sizeof(size_t));
    if (new_targets == NULL) {
      janet_panic("JIT could not allocate while generating jump placeholders");
    }
    code->jump_targets = new_targets;

    size_t *new_locations =
      realloc(code->jump_locations, new_capacity * sizeof(size_t));
    if (new_locations == NULL) {
      janet_panic("JIT could not allocate while generating jump placeholders");
    }
    code->jump_locations = new_locations;
    code->jump_capacity = new_capacity;
  }

  code->jump_targets[code->jump_index] = target_bb;
  code->jump_locations[code->jump_index] = code->count;
  code->jump_index++;
  emit_u32(code, 0);
}

// TODO: this also needs to take operands
static void emit_binary_fallback(CodeBuffer *code, MethodBlocks *blocks,
                                 Instruction *instr, JitBinaryFallback fallback) {
  emit_op_to_arg(code, 0, &blocks->vregs[instr->args[0]]);
  emit_op_to_arg(code, 1, &blocks->vregs[instr->args[1]]);
  emit_cfun_call(code, blocks, instr, fallback);
  emit_rax_to_op(code, &blocks->vregs[instr->result]);
}

static void emit_binary_imm_fallback(CodeBuffer *code, MethodBlocks *blocks,
                                     Instruction *instr, Operand *imm,
                                     JitBinaryFallback fallback) {
  emit_op_to_arg(code, 0, &blocks->vregs[instr->args[0]]);
  emit_op_to_arg(code, 1, imm);
  emit_cfun_call(code, blocks, instr, fallback);
  emit_rax_to_op(code, &blocks->vregs[instr->result]);
}

static void emit_unary_fallback(CodeBuffer *code, MethodBlocks *blocks,
                                Instruction *instr, JitUnaryFallback fallback) {
  emit_op_to_arg(code, 0, &blocks->vregs[instr->args[0]]);
  emit_cfun_call(code, blocks, instr, fallback);
  emit_rax_to_op(code, &blocks->vregs[instr->result]);
}

void emit_block(CodeBuffer *code, MethodBlocks *blocks, uint32_t stack_size, uint32_t block_id) {
  blocks->block_locations[block_id] = code->count;
  int call_args_loc = stack_size - sizeof(CallArgs*);
  BasicBlock *block = &blocks->blocks[block_id];
  for (size_t instr_i = 0; instr_i < block->count; instr_i++) {
    Instruction *instr = &block->instructions[instr_i];
    switch (instr->type) {
    case HIR_NOOP: {
      break;
    }
    case HIR_ERROR: {
      emit_op_to_arg(code, 0, &blocks->vregs[instr->args[0]]);
      emit_cfun_call(code, blocks, instr, janet_panicv);
      break;
    }
    case HIR_TYPECHECK: {
      emit_op_to_arg(code, 0, &blocks->vregs[instr->args[0]]);
      emit_imm_to_arg(code, 1, blocks->imms[instr->args[1]].immus);
      emit_cfun_call(code, blocks, instr, jit_typecheck);
      break;
    }
    case HIR_RETURN: {
      emit_op_to_gpr(code, 0, &blocks->vregs[instr->args[0]]);
      if (stack_size > 0) {
	emit_byte(code, 0x48);
	emit_byte(code, 0x81);
	emit_byte(code, 0xC4);
	emit_u32(code, stack_size);
      }
      emit_byte(code, 0xC3);
      break;
    }
    case HIR_RETURN_NIL: {
      emit_imm_gpr(code, 0, blocks->imms[instr->args[0]].immus);
      if (stack_size > 0) {
	emit_byte(code, 0x48);
	emit_byte(code, 0x81);
	emit_byte(code, 0xC4);
	emit_u32(code, stack_size);
      }
      emit_byte(code, 0xC3);
      break;
    }
    case HIR_ADD: {
      if (operands_numeric(blocks->virtual_register_types, &blocks->vregs[instr->args[0]], &blocks->vregs[instr->args[1]])) {
	emit_binary_op_op(code, X86_ADD, &blocks->vregs[instr->result],
			  &blocks->vregs[instr->args[0]], &blocks->vregs[instr->args[1]]);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_add_fallback);
      }
      break;
    }
    case HIR_ADD_IMM: {
      if (operands_numeric(blocks->virtual_register_types, &blocks->vregs[instr->args[0]], &blocks->imms[instr->args[1]])) {
	emit_binary_op_op(code, X86_ADD, &blocks->vregs[instr->result],
			  &blocks->vregs[instr->args[0]], &blocks->imms[instr->args[1]]);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_add_fallback);
      }
      break;
    }
    case HIR_SUB: {
      if (operands_numeric(blocks->virtual_register_types, &blocks->vregs[instr->args[0]], &blocks->vregs[instr->args[1]])) {
	emit_binary_op_op(code, X86_SUB, &blocks->vregs[instr->result],
			  &blocks->vregs[instr->args[0]], &blocks->vregs[instr->args[1]]);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_sub_fallback);
      }
      break;
    }
    case HIR_SUB_IMM: {
      if (operands_numeric(blocks->virtual_register_types, &blocks->vregs[instr->args[0]], &blocks->imms[instr->args[1]])) {
	emit_binary_op_op(code, X86_SUB, &blocks->vregs[instr->result],
			  &blocks->vregs[instr->args[0]], &blocks->imms[instr->args[1]]);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_sub_fallback);
      }
      break;
    }
    case HIR_MUL: {
      if (operands_numeric(blocks->virtual_register_types, &blocks->vregs[instr->args[0]], &blocks->vregs[instr->args[1]])) {
	emit_binary_op_op(code, X86_MUL,
			  &blocks->vregs[instr->result],
			  &blocks->vregs[instr->args[0]],
			  &blocks->vregs[instr->args[1]]);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_mul_fallback);
      }
      break;
    }
    case HIR_MUL_IMM: {
      if (operands_numeric(blocks->virtual_register_types, &blocks->vregs[instr->args[0]], &blocks->imms[instr->args[1]])) {
	emit_binary_op_op(code, X86_MUL, &blocks->vregs[instr->result],
			  &blocks->vregs[instr->args[0]], &blocks->imms[instr->args[1]]);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_mul_fallback);
      }
      break;
    }
    case HIR_DIV: {
      if (operands_numeric(blocks->virtual_register_types, &blocks->vregs[instr->args[0]], &blocks->vregs[instr->args[1]])) {
	emit_binary_op_op(code, X86_DIV, &blocks->vregs[instr->result],
			  &blocks->vregs[instr->args[0]], &blocks->vregs[instr->args[1]]);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_div_fallback);
      }
      break;
    }
    case HIR_DIV_IMM: {
      if (operands_numeric(blocks->virtual_register_types, &blocks->vregs[instr->args[0]], &blocks->imms[instr->args[1]])) {
	emit_binary_op_op(code, X86_DIV, &blocks->vregs[instr->result],
			  &blocks->vregs[instr->args[0]], &blocks->imms[instr->args[1]]);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_div_fallback);
      }
      break;
    }
    case HIR_DIV_FLOOR: {
      if (operands_numeric(blocks->virtual_register_types, &blocks->vregs[instr->args[0]], &blocks->vregs[instr->args[1]])) {
	emit_op_to_xmm(code, 0, &blocks->vregs[instr->args[0]]);
	emit_op_to_xmm(code, 1, &blocks->vregs[instr->args[1]]);
	emit_binary_op(code, X86_DIV, 0, 0, 1);
	emit_floor(code, 0);
	emit_xmm_to_op(code, &blocks->vregs[instr->result], 0);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_divf_fallback);
      }
      break;
    }
    case HIR_MODULO: {
      if (operands_numeric(blocks->virtual_register_types, &blocks->vregs[instr->args[0]], &blocks->vregs[instr->args[1]])) {
	emit_op_to_xmm(code, 0, &blocks->vregs[instr->args[0]]);
	emit_op_to_xmm(code, 1, &blocks->vregs[instr->args[0]]);
	emit_op_to_xmm(code, 2, &blocks->vregs[instr->args[1]]);
	emit_binary_op(code, X86_DIV, 1, 1, 2);
	emit_floor(code, 1);
	emit_binary_op(code, X86_MUL, 1, 1, 2);
	emit_binary_op(code, X86_SUB, 0, 0, 1);
	emit_xmm_to_op(code, &blocks->vregs[instr->result], 0);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_mod_fallback);
      }
      break;
    }
    case HIR_REMAINDER: {
      if (operands_numeric(blocks->virtual_register_types, &blocks->vregs[instr->args[0]], &blocks->vregs[instr->args[1]])) {
	emit_op_to_xmm(code, 0, &blocks->vregs[instr->args[0]]);
	emit_op_to_xmm(code, 1, &blocks->vregs[instr->args[0]]);
	emit_op_to_xmm(code, 2, &blocks->vregs[instr->args[1]]);
	emit_binary_op(code, X86_DIV, 1, 1, 2);
	emit_trunc(code, 1);
	emit_binary_op(code, X86_MUL, 1, 1, 2);
	emit_binary_op(code, X86_SUB, 0, 0, 1);
	emit_xmm_to_op(code, &blocks->vregs[instr->result], 0);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_rem_fallback);
      }
      break;
    }
    case HIR_AND: {
      if (operands_numeric(blocks->virtual_register_types, &blocks->vregs[instr->args[0]], &blocks->vregs[instr->args[1]])) {
	// Should do I do range checks here, or just say, "The jit rolls over"
	emit_op_to_integer_gpr(code, 0, &blocks->vregs[instr->args[0]]);
	emit_op_to_integer_gpr(code, 1, &blocks->vregs[instr->args[1]]);
	emit_gpr_op(code, X86_AND_GPR, 0, 1);
	emit_gpr_to_number_op(code, &blocks->vregs[instr->result], 0);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_band_fallback);
      }
      break;
    }
    case HIR_OR: {
      if (operands_numeric(blocks->virtual_register_types, &blocks->vregs[instr->args[0]], &blocks->vregs[instr->args[1]])) {
	emit_op_to_integer_gpr(code, 0, &blocks->vregs[instr->args[0]]);
	emit_op_to_integer_gpr(code, 1, &blocks->vregs[instr->args[1]]);
	emit_gpr_op(code, X86_OR_GPR, 0, 1);
	emit_gpr_to_number_op(code, &blocks->vregs[instr->result], 0);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_bor_fallback);
      }
      break;
    }
    case HIR_XOR: {
      if (operands_numeric(blocks->virtual_register_types, &blocks->vregs[instr->args[0]], &blocks->vregs[instr->args[1]])) {
	emit_op_to_integer_gpr(code, 0, &blocks->vregs[instr->args[0]]);
	emit_op_to_integer_gpr(code, 1, &blocks->vregs[instr->args[1]]);
	emit_gpr_op(code, X86_XOR_GPR, 0, 1);
	emit_gpr_to_number_op(code, &blocks->vregs[instr->result], 0);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_bxor_fallback);
      }
      break;
    }
    case HIR_NOT: {
      if (operand_numeric(blocks->virtual_register_types, &blocks->vregs[instr->args[0]])) {
	emit_op_to_integer_gpr(code, 0, &blocks->vregs[instr->args[0]]);
	emit_not(code, 0);
	emit_gpr_to_number_op(code, &blocks->vregs[instr->result], 0);
      } else {
	emit_unary_fallback(code, blocks, instr, jit_bnot_fallback);
      }
      break;
    }
    case HIR_LSHIFT: {
      if (operands_numeric(blocks->virtual_register_types,
			   &blocks->vregs[instr->args[0]],
			   &blocks->vregs[instr->args[1]])) {
	emit_shift(code,
		   X86_SHIFT_LEFT,
		   true,
		   &blocks->vregs[instr->result],
		   &blocks->vregs[instr->args[0]],
		   &blocks->vregs[instr->args[1]]);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_blshift_fallback);
      }
      break;
    }
    case HIR_LSHIFT_IMM: {
      if (operands_numeric(blocks->virtual_register_types,
			   &blocks->vregs[instr->args[0]],
			   &blocks->imms[instr->args[1]])) {
	emit_shift(code,
		   X86_SHIFT_LEFT,
		   true,
		   &blocks->vregs[instr->result],
		   &blocks->vregs[instr->args[0]],
		   &blocks->imms[instr->args[1]]);
      } else {
	emit_binary_imm_fallback(code, blocks, instr, &blocks->imms[instr->args[1]], jit_blshift_fallback);
      }
      break;
    }
    case HIR_RSHIFT: {
      if (operands_numeric(blocks->virtual_register_types,
			   &blocks->vregs[instr->args[0]],
			   &blocks->vregs[instr->args[1]])) {
	emit_shift(code,
		   X86_SHIFT_RIGHT_SIGNED,
		   true,
		   &blocks->vregs[instr->result],
		   &blocks->vregs[instr->args[0]],
		   &blocks->vregs[instr->args[1]]);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_brshift_fallback);
      }
      break;
    }
    case HIR_RSHIFT_IMM: {
      if (operands_numeric(blocks->virtual_register_types,
			   &blocks->vregs[instr->args[0]],
			   &blocks->imms[instr->args[1]])) {
	emit_shift(code,
		   X86_SHIFT_RIGHT_SIGNED,
		   true,
		   &blocks->vregs[instr->result],
		   &blocks->vregs[instr->args[0]],
		   &blocks->imms[instr->args[1]]);
      } else {
	emit_binary_imm_fallback(code, blocks, instr, &blocks->imms[instr->args[1]], jit_brshift_fallback);
      }
      break;
    }
    case HIR_RUSHIFT: {
      if (operands_numeric(blocks->virtual_register_types,
			   &blocks->vregs[instr->args[0]],
			   &blocks->vregs[instr->args[1]])) {
	emit_shift(code,
		   X86_SHIFT_RIGHT_UNSIGNED,
		   false,
		   &blocks->vregs[instr->result],
		   &blocks->vregs[instr->args[0]],
		   &blocks->vregs[instr->args[1]]);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_brushift_fallback);
      }
      break;
    }
    case HIR_RUSHIFT_IMM: {
      if (operands_numeric(blocks->virtual_register_types,
			   &blocks->vregs[instr->args[0]],
			   &blocks->imms[instr->args[1]])) {
	emit_shift(code,
		   X86_SHIFT_RIGHT_UNSIGNED,
		   false,
		   &blocks->vregs[instr->result],
		   &blocks->vregs[instr->args[0]],
		   &blocks->imms[instr->args[1]]);
      } else {
	emit_binary_imm_fallback(code, blocks, instr, &blocks->imms[instr->args[1]], jit_brushift_fallback);
      }
      break;
    }
    case HIR_JUMP: {
      emit_phi_moves(code, blocks, block_id, blocks->bbs[instr->args[0]].bb);
      emit_byte(code, 0xE9); // unconditional jump
      emit_jump_placeholder(code, code->count, blocks->bbs[instr->args[0]].bb);
      break;
    }
    case HIR_JUMP_IF: {
      emit_op_to_arg(code, 0, &blocks->vregs[instr->args[2]]);
      emit_cfun_call(code, blocks, instr, janet_truthy);
      // compare
      emit_byte(code, 0x85);
      emit_byte(code, 0xC0);
      // conditional jump
      emit_byte(code, 0x0F);
      emit_byte(code, 0x84); // JZ
      // no target basic block because we will fill it in right now
      // with the jump case phi resolutions

      // manual jump placeholder since we fill it in right away.
      size_t internal_jump_pc = code->count;
      emit_u32(code, 0);

      // phis for if not case
      emit_phi_moves(code, blocks, block_id, blocks->bbs[instr->args[0]].bb);
      emit_byte(code, 0xE9); // unconditional jump
      emit_jump_placeholder(code, code->count, blocks->bbs[instr->args[0]].bb);

      edit_u32(code, internal_jump_pc, code->count - (internal_jump_pc + 4));
      // phis for conditional false destination <A>
      emit_phi_moves(code, blocks, block_id, blocks->bbs[instr->args[1]].bb);
      emit_byte(code, 0xE9); // unconditional jump
      emit_jump_placeholder(code, code->count, blocks->bbs[instr->args[1]].bb);
      break;
    }
    case HIR_JUMP_IF_NOT: {
      // jump if conditional true to <A>
      emit_op_to_arg(code, 0, &blocks->vregs[instr->args[2]]);
      emit_cfun_call(code, blocks, instr, janet_truthy);
      // compare
      emit_byte(code, 0x85);
      emit_byte(code, 0xC0);
      // conditional jump
      emit_byte(code, 0x0F);
      emit_byte(code, 0x85); // JNZ
      // no target basic block because we will fill it in right now
      // with the jump case phi resolutions

      // manual jump placeholder since we fill it in right away.
      size_t internal_jump_pc = code->count;
      emit_u32(code, 0);

      // phis for if not case
      emit_phi_moves(code, blocks, block_id, blocks->bbs[instr->args[0]].bb);
      emit_byte(code, 0xE9); // unconditional jump
      emit_jump_placeholder(code, code->count, blocks->bbs[instr->args[0]].bb);

      edit_u32(code, internal_jump_pc, code->count - (internal_jump_pc + 4));
      // phis for conditional false destination <A>
      emit_phi_moves(code, blocks, block_id, blocks->bbs[instr->args[1]].bb);
      emit_byte(code, 0xE9); // unconditional jump
      emit_jump_placeholder(code, code->count, blocks->bbs[instr->args[1]].bb);
      break;
    }
    case HIR_JUMP_IF_NIL: {
      // jump if conditional true to <A>
      emit_op_to_arg(code, 0, &blocks->vregs[instr->args[2]]);
      emit_cfun_call(code, blocks, instr, jit_nil);
      // compare
      emit_byte(code, 0x85);
      emit_byte(code, 0xC0);
      // conditional jump
      emit_byte(code, 0x0F);
      emit_byte(code, 0x84); // JZ
      // no target basic block because we will fill it in right now
      // with the jump case phi resolutions

      // manual jump placeholder since we fill it in right away.
      size_t internal_jump_pc = code->count;
      emit_u32(code, 0);

      // phis for if not case
      emit_phi_moves(code, blocks, block_id, blocks->bbs[instr->args[0]].bb);
      emit_byte(code, 0xE9); // unconditional jump
      emit_jump_placeholder(code, code->count, blocks->bbs[instr->args[0]].bb);

      edit_u32(code, internal_jump_pc, code->count - (internal_jump_pc + 4));
      // phis for conditional false destination <A>
      emit_phi_moves(code, blocks, block_id, blocks->bbs[instr->args[1]].bb);
      emit_byte(code, 0xE9); // unconditional jump
      emit_jump_placeholder(code, code->count, blocks->bbs[instr->args[1]].bb);
      break;
    }
    case HIR_JUMP_IF_NOT_NIL: {
      // jump if conditional true to <A>
      emit_op_to_arg(code, 0, &blocks->vregs[instr->args[2]]);
      emit_cfun_call(code, blocks, instr, jit_nil);
      // compare
      emit_byte(code, 0x85);
      emit_byte(code, 0xC0);
      // conditional jump
      emit_byte(code, 0x0F);
      emit_byte(code, 0x85); // JNZ
      // no target basic block because we will fill it in right now
      // with the jump case phi resolutions

      // manual jump placeholder since we fill it in right away.
      size_t internal_jump_pc = code->count;
      emit_u32(code, 0);

      // phis for if not case
      emit_phi_moves(code, blocks, block_id, blocks->bbs[instr->args[0]].bb);
      emit_byte(code, 0xE9); // unconditional jump
      emit_jump_placeholder(code, code->count, blocks->bbs[instr->args[0]].bb);

      edit_u32(code, internal_jump_pc, code->count - (internal_jump_pc + 4));
      // phis for conditional false destination <A>
      emit_phi_moves(code, blocks, block_id, blocks->bbs[instr->args[1]].bb);
      emit_byte(code, 0xE9); // unconditional jump
      emit_jump_placeholder(code, code->count, blocks->bbs[instr->args[1]].bb);
      break;
    }
    case HIR_GREATER_THAN: {
      if (operands_numeric(blocks->virtual_register_types,
			   &blocks->vregs[instr->args[0]],
			   &blocks->vregs[instr->args[1]])) {
	emit_comparison(code,
			&blocks->vregs[instr->args[0]],
			&blocks->vregs[instr->args[1]],
			false,
			X86_CMOV_ABOVE,
			-1,
			&blocks->vregs[instr->result]);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_greater_than_fallback);
      }
      break;
    }
    case HIR_GREATER_THAN_IMM: {
      if (operands_numeric(blocks->virtual_register_types,
			   &blocks->vregs[instr->args[0]],
			   &blocks->imms[instr->args[1]])) {
	emit_comparison(code,
			&blocks->vregs[instr->args[0]],
			&blocks->imms[instr->args[1]],
			false,
			X86_CMOV_ABOVE,
			-1,
			&blocks->vregs[instr->result]);
      } else {
	emit_binary_imm_fallback(code, blocks, instr, &blocks->imms[instr->args[1]], jit_greater_than_fallback);
      }
      break;
    }
    case HIR_LESS_THAN: {
      if (operands_numeric(blocks->virtual_register_types,
			   &blocks->vregs[instr->args[0]],
			   &blocks->vregs[instr->args[1]])) {
	emit_comparison(code,
			&blocks->vregs[instr->args[0]],
			&blocks->vregs[instr->args[1]],
			true,
			X86_CMOV_ABOVE,
			-1,
			&blocks->vregs[instr->result]);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_less_than_fallback);
      }
      break;
    }
    case HIR_LESS_THAN_IMM: {
      if (operands_numeric(blocks->virtual_register_types,
			   &blocks->vregs[instr->args[0]],
			   &blocks->imms[instr->args[1]])) {
	emit_comparison(code,
			&blocks->vregs[instr->args[0]],
			&blocks->imms[instr->args[1]],
			true,
			X86_CMOV_ABOVE,
			-1,
			&blocks->vregs[instr->result]);
      } else {
	emit_binary_imm_fallback(code, blocks, instr, &blocks->imms[instr->args[1]], jit_less_than_fallback);
      }
      break;
    }
    case HIR_EQUALS: {
      if (operands_numeric(blocks->virtual_register_types,
			   &blocks->vregs[instr->args[0]],
			   &blocks->vregs[instr->args[1]])) {
	emit_comparison(code,
			&blocks->vregs[instr->args[0]],
			&blocks->vregs[instr->args[1]],
			false,
			X86_CMOV_EQUAL,
			false,
			&blocks->vregs[instr->result]);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_equals_fallback);
      }
      break;
    }
    case HIR_EQUALS_IMM: {
      if (operands_numeric(blocks->virtual_register_types,
			   &blocks->vregs[instr->args[0]],
			   &blocks->imms[instr->args[1]])) {
	emit_comparison(code,
			&blocks->vregs[instr->args[0]],
			&blocks->imms[instr->args[1]],
			false,
			X86_CMOV_EQUAL,
			false,
			&blocks->vregs[instr->result]);
      } else {
	emit_binary_imm_fallback(code, blocks, instr, &blocks->imms[instr->args[1]], jit_equals_fallback);
      }
      break;
    }
    case HIR_COMPARE: {
      if (operands_numeric(blocks->virtual_register_types,
			   &blocks->vregs[instr->args[0]],
			   &blocks->vregs[instr->args[1]])) {
	emit_compare(code,
		     &blocks->vregs[instr->args[0]],
		     &blocks->vregs[instr->args[1]],
		     &blocks->vregs[instr->result]);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_compare_fallback);
      }
      break;
    }
    case HIR_LOAD: {
      emit_store_imm_op(code, &blocks->vregs[instr->result], &blocks->imms[instr->args[0]]);
      break;
    }
    case HIR_LOAD_ARG: {
      if (blocks->vregs[instr->result].physical_location.on_stack == false) {
	emit_arg_to_xmm(code, blocks->imms[instr->args[0]].immus, blocks->vregs[instr->result].physical_location.reg);
      } else {
	emit_arg_to_stack(code, blocks->imms[instr->args[0]].immus, instr->result);
      }
      break;
    }
    case HIR_PUSH: {
      emit_non_janet_to_arg(code, 0, call_args_loc);
      emit_op_to_arg(code, 1, &blocks->vregs[instr->args[0]]);
      emit_cfun_call(code, blocks, instr, jit_push);
      break;
    }
    case HIR_PUSH2: {
      emit_non_janet_to_arg(code, 0, call_args_loc);
      emit_op_to_arg(code, 1, &blocks->vregs[instr->args[0]]);
      emit_op_to_arg(code, 2, &blocks->vregs[instr->args[1]]);
      emit_cfun_call(code, blocks, instr, jit_push_2);
      break;
    }
    case HIR_PUSH3: {
      emit_non_janet_to_arg(code, 0, call_args_loc);
      emit_op_to_arg(code, 1, &blocks->vregs[instr->args[0]]);
      emit_op_to_arg(code, 2, &blocks->vregs[instr->args[1]]);
      emit_op_to_arg(code, 3, &blocks->vregs[instr->args[2]]);
      emit_cfun_call(code, blocks, instr, jit_push_3);
      break;
    }
    case HIR_CALL: {
      emit_op_to_arg(code, 0, &blocks->vregs[instr->args[0]]);
      emit_non_janet_to_arg(code, 1, call_args_loc);
      emit_cfun_call(code, blocks, instr, jit_call);
      emit_rax_to_op(code, &blocks->vregs[instr->result]);
      break;
    }
    case HIR_TAIL_CALL: {
      emit_op_to_arg(code, 0, &blocks->vregs[instr->args[0]]);
      emit_non_janet_to_arg(code, 1, call_args_loc);
      emit_cfun_call(code, blocks, instr, jit_call);
      if (stack_size > 0) {
	// stack adjust
	emit_byte(code, 0x48);
	emit_byte(code, 0x81);
	emit_byte(code, 0xC4);
	emit_u32(code, stack_size);
      }
      emit_byte(code, 0xC3); // ret
      break;
    }
    case HIR_IN: {
      emit_op_to_arg(code, 0, &blocks->vregs[instr->args[0]]);
      emit_op_to_arg(code, 1, &blocks->vregs[instr->args[1]]);
      emit_cfun_call(code, blocks, instr, janet_in);
      emit_rax_to_op(code, &blocks->vregs[instr->result]);
      break;
    }
    case HIR_GET: {
      emit_op_to_arg(code, 0, &blocks->vregs[instr->args[0]]);
      emit_op_to_arg(code, 1, &blocks->vregs[instr->args[1]]);
      emit_cfun_call(code, blocks, instr, janet_get);
      emit_rax_to_op(code, &blocks->vregs[instr->result]);
      break;
    }
    case HIR_GET_INDEX: {
      emit_op_to_arg(code, 0, &blocks->vregs[instr->args[0]]);
      emit_imm_to_arg(code, 1, blocks->imms[instr->args[1]].immus);
      emit_cfun_call(code, blocks, instr, janet_getindex);
      emit_rax_to_op(code, &blocks->vregs[instr->result]);
      break;
    }
    case HIR_PUT: {
      emit_op_to_arg(code, 0, &blocks->vregs[instr->args[0]]);
      emit_op_to_arg(code, 1, &blocks->vregs[instr->args[1]]);
      emit_op_to_arg(code, 2, &blocks->vregs[instr->args[2]]);
      emit_cfun_call(code, blocks, instr, janet_put);
      break;
    }
    case HIR_PUT_INDEX: {
      emit_op_to_arg(code, 0, &blocks->vregs[instr->args[0]]);
      emit_imm_to_arg(code, 1, operand_to_janet_bits(&blocks->imms[instr->args[1]]));
      emit_op_to_arg(code, 2, &blocks->vregs[instr->args[2]]);
      emit_cfun_call(code, blocks, instr, janet_put);
      break;
    }
    case HIR_LENGTH: {
      emit_op_to_arg(code, 0, &blocks->vregs[instr->args[0]]);
      emit_cfun_call(code, blocks, instr, janet_lengthv);
      emit_rax_to_op(code, &blocks->vregs[instr->result]);
      break;
    }
    case HIR_MAKE_ARRAY:
      emit_non_janet_to_arg(code, 0, call_args_loc);
      emit_cfun_call(code, blocks, instr, jit_make_array);
      emit_rax_to_op(code, &blocks->vregs[instr->result]);
      break;
    case HIR_MAKE_TUPLE:
      emit_non_janet_to_arg(code, 0, call_args_loc);
      emit_cfun_call(code, blocks, instr, jit_make_tuple);
      emit_rax_to_op(code, &blocks->vregs[instr->result]);
      break;
    case HIR_MAKE_BRACKET_TUPLE:
      emit_non_janet_to_arg(code, 0, call_args_loc);
      emit_cfun_call(code, blocks, instr, jit_make_bracket_tuple);
      emit_rax_to_op(code, &blocks->vregs[instr->result]);
      break;
    case HIR_MAKE_BUFFER:
      emit_non_janet_to_arg(code, 0, call_args_loc);
      emit_cfun_call(code, blocks, instr, jit_make_buffer);
      emit_rax_to_op(code, &blocks->vregs[instr->result]);
      break;
    case HIR_MAKE_STRING:
      emit_non_janet_to_arg(code, 0, call_args_loc);
      emit_cfun_call(code, blocks, instr, jit_make_string);
      emit_rax_to_op(code, &blocks->vregs[instr->result]);
      break;
    case HIR_MAKE_STRUCT:
      emit_non_janet_to_arg(code, 0, call_args_loc);
      emit_cfun_call(code, blocks, instr, jit_make_struct);
      emit_rax_to_op(code, &blocks->vregs[instr->result]);
      break;
    case HIR_MAKE_TABLE:
      emit_non_janet_to_arg(code, 0, call_args_loc);
      emit_cfun_call(code, blocks, instr, jit_make_table);
      emit_rax_to_op(code, &blocks->vregs[instr->result]);
      break;
    case HIR_GREATER_THAN_EQUAL: {
      if (operands_numeric(blocks->virtual_register_types,
			   &blocks->vregs[instr->args[0]],
			   &blocks->vregs[instr->args[1]])) {
	emit_comparison(code,
			&blocks->vregs[instr->args[0]],
			&blocks->vregs[instr->args[1]],
			false,
			X86_CMOV_ABOVE_EQUAL,
			-1,
			&blocks->vregs[instr->result]);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_greater_than_equal_fallback);
      }
      break;
    }
    case HIR_LESS_THAN_EQUAL: {
      if (operands_numeric(blocks->virtual_register_types,
			   &blocks->vregs[instr->args[0]],
			   &blocks->vregs[instr->args[1]])) {
	emit_comparison(code,
			&blocks->vregs[instr->args[0]],
			&blocks->vregs[instr->args[1]],
			true,
			X86_CMOV_ABOVE_EQUAL,
			-1,
			&blocks->vregs[instr->result]);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_less_than_equal_fallback);
      }
      break;
    }
    case HIR_NEXT: {
      emit_op_to_arg(code, 0, &blocks->vregs[instr->args[0]]);
      emit_op_to_arg(code, 1, &blocks->vregs[instr->args[1]]);
      emit_cfun_call(code, blocks, instr, janet_next);
      emit_rax_to_op(code, &blocks->vregs[instr->result]);
      break;
    }
    case HIR_NOT_EQUALS: {
      if (operands_numeric(blocks->virtual_register_types,
			   &blocks->vregs[instr->args[0]],
			   &blocks->vregs[instr->args[1]])) {
	emit_comparison(code,
			&blocks->vregs[instr->args[0]],
			&blocks->vregs[instr->args[1]],
			false,
			X86_CMOV_NOT_EQUAL,
			true,
			&blocks->vregs[instr->result]);
      } else {
	emit_binary_fallback(code, blocks, instr, jit_not_equals_fallback);
      }
      break;
    }
    case HIR_NOT_EQUALS_IMM: {
      if (operands_numeric(blocks->virtual_register_types,
			   &blocks->vregs[instr->args[0]],
			   &blocks->imms[instr->args[1]])) {
	emit_comparison(code,
			&blocks->vregs[instr->args[0]],
			&blocks->imms[instr->args[1]],
			false,
			X86_CMOV_NOT_EQUAL,
			true,
			&blocks->vregs[instr->result]);
      } else {
	emit_binary_imm_fallback(code, blocks, instr, &blocks->imms[instr->args[1]], jit_not_equals_fallback);
      }
      break;
    }
    case HIR_PHI: {
      // noop -- should move these to the edge
      break;
    }
    default:
      janet_panicf("unsupported op! %s", instruction_names[instr->type]);
    }
  }
}

static void compile(JittedFunction *jitted) {
  CodeBuffer code = {
    .count = 0,
    .capacity = 256,
    .data = malloc(256 * sizeof(uint8_t)),
    .jump_index = 0,
    .jump_capacity = 256,
    .jump_targets = malloc(256 * sizeof(size_t)),
    .jump_locations = malloc(256 * sizeof(size_t)),
  };

  // stack size rounded to 16
  uint32_t stack_size = // align_up(jitted->method_blocks.vreg_count * sizeof(Janet) + sizeof(CallArgs*), 16) + 8
    ((jitted->method_blocks.vreg_count * sizeof(Janet) + sizeof(CallArgs*)) + 7u & ~15u) + 8u;

  if (stack_size > 0) {
    emit_byte(&code, 0x48);             // REX.W: use 64-bit operands.
    emit_byte(&code, 0x81);             // Group 1 arithmetic on r/m64 with imm32.
    emit_byte(&code, 0xEC);             // 0xEC: sub rsp, imm32; 0xC4: add rsp, imm32.
    emit_u32(&code, stack_size); // Stack-frame size.
  }

  emit_arg_to_frame(&code, 1, stack_size - sizeof(CallArgs*));

  for (size_t block_i = 0; block_i < jitted->method_blocks.count; block_i++) {
    if (jitted->method_blocks.blocks[block_i].complete) {
      emit_block(&code, &jitted->method_blocks, stack_size, block_i);
    }
  }

  // resolve jump locations
  for (size_t jump_i = 0; jump_i < code.jump_index; jump_i++) {
    size_t target_loc = code.jump_targets[jump_i];
    size_t code_loc = code.jump_locations[jump_i];
    size_t target_location = jitted->method_blocks.block_locations[target_loc];
    edit_u32(&code, code_loc, target_location - (code_loc + 4));
  }

  void * mapping = mmap(NULL,
			code.count,
			PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS,
			-1,
			0);

  if (mapping == MAP_FAILED) {
    int error = errno;
    janet_panicf("could not allocate JIT memory: %s", strerror(error));
  }

  memcpy(mapping, code.data, code.count);

  if (mprotect(mapping, code.count, PROT_READ | PROT_EXEC) != 0) {
    int error = errno;
    munmap(mapping, code.count);
    janet_panicf("could not make JIT memory executable: %s", strerror(error));
  }

  free(code.data);
  jitted->code_size = code.count;
  jitted->code = mapping;

  free(code.jump_targets);
  free(code.jump_locations);
}

static void free_method_blocks(MethodBlocks *blocks) {
  for (size_t block_i = 0; block_i < blocks->count; block_i++) {
    BasicBlock *block = &blocks->blocks[block_i];

    for (size_t instr_i = 0; instr_i < block->count; instr_i++) {
      free(block->instructions[instr_i].phi_sources);
    }

    free(block->instructions);
    free(block->virtual_register_def);
    free(block->virtual_register_last_use);
    free(block->slot_map);
    free(block->input_edges);
    free(block->output_edges);
    free(block->slot_use);
    free(block->slot_def);
    free(block->live_in);
    free(block->live_out);
  }

  free(blocks->blocks);
  free(blocks->block_start_pcs);
  free(blocks->block_locations);
  free(blocks->vregs);
  free(blocks->imms);
  free(blocks->bbs);
  free(blocks->virtual_register_types);
  free(blocks->vreg_defs);
  free(blocks->vreg_last_uses);

  *blocks = (MethodBlocks) {0};
}

static int jitted_function_gc(void *p, size_t size) {
  JittedFunction *jitted = p;
  (void)size;
  if (jitted->code != NULL) {
    munmap(jitted->code, jitted->code_size);
  }
  if (jitted->signature_arg_types != NULL) {
    free(jitted->signature_arg_types);
  }

  if (jitted->ca.capacity > 0) {
    free(jitted->ca.argv);
  }
  free_method_blocks(&jitted->method_blocks);
  return 0;
}

static int jitted_function_gcmark(void *p, size_t size) {
  JittedFunction *jitted = p;
  (void)size;
  janet_mark(janet_wrap_function(jitted->fallback));
  for (size_t i = 0; i < jitted->ca.count; i++) {
    janet_mark(jitted->ca.argv[i]);
  }
  return 0;
}

static Janet handle_mismatch(JittedFunction *jitted, int32_t argc, Janet *argv) {
  switch (jitted->mismatch_behavior) {
  case MISMATCH_FALLBACK:
    return janet_call(jitted->fallback, argc, argv);
  case MISMATCH_ERROR:
    janet_panic("mismatching signature!");
    break;
  case MISMATCH_RECOMPILE:
    janet_panic("recompile not yet supported!");
  }
}

static Janet jitted_op_tuple(Operand *op) {
  Janet *janet_op = janet_tuple_begin(2);
  janet_op[0] = janet_ckeywordv(operand_names[op->type]);
  switch (op->type) {
  case OPERAND_VIRTUAL_REGISTER:
    janet_op[1] = janet_wrap_number(op->virtual_register);
    break;
  case OPERAND_BASIC_BLOCK:
    janet_op[1] = janet_wrap_number(op->bb);
    break;
  case OPERAND_SIGNED_IMM:
    janet_op[1] = janet_wrap_number(op->imms);
    break;
  case OPERAND_UNSIGNED_IMM:
    janet_op[1] = janet_wrap_number(op->immus);
    break;
  case OPERAND_JANET_IMM: {
    Janet value;
    value.u64 = op->immus;
    janet_op[1] = value;
    break;
  }
  case OPERAND_UNUSED:
    janet_op[1] = janet_ckeywordv("_");
    break;
  case OPERAND_UNDEFINED:
    janet_op[1] = janet_ckeywordv("?");
    break;
  }
  return janet_wrap_tuple(janet_tuple_end(janet_op));
}

static Janet jitted_janet_hir(JittedFunction *jitted) {
  MethodBlocks *blocks = &jitted->method_blocks;
  Janet *janet_bbs = janet_tuple_begin(blocks->count);
  for (size_t block_i = 0; block_i < blocks->count; block_i++) {
    BasicBlock *block = &blocks->blocks[block_i];
    Janet *janet_bb_instrs = janet_tuple_begin(block->count);
    for (size_t instr_i = 0; instr_i < block->count; instr_i++) {
      Instruction *instr = &block->instructions[instr_i];
      if (instr->type == HIR_PHI) {
	JanetTable *janet_instr = janet_table(5 + instr->phi_source_count);
	janet_table_put(janet_instr, janet_ckeywordv("type"), janet_ckeywordv(instruction_names[instr->type]));
	janet_table_put(janet_instr, janet_ckeywordv("result"), jitted_op_tuple(&blocks->vregs[instr->result]));

	Janet *janet_args = janet_tuple_begin(3);
	janet_args[0] = janet_ckeywordv("TODO");

	janet_table_put(janet_instr, janet_ckeywordv("args"), janet_wrap_tuple(janet_tuple_end(janet_args)));
	for (size_t i = 0; i < instr->phi_source_count; i++) {
	  PhiSource *source = &instr->phi_sources[i];
	  janet_table_put(janet_instr, janet_wrap_number(source->bb), jitted_op_tuple(&blocks->vregs[source->virtual_register]));
	}
	janet_bb_instrs[instr_i] = janet_wrap_struct(janet_table_to_struct(janet_instr));
      } else {
	JanetTable *janet_instr = janet_table(5);
	janet_table_put(janet_instr, janet_ckeywordv("type"), janet_ckeywordv(instruction_names[instr->type]));
	janet_table_put(janet_instr, janet_ckeywordv("result"), jitted_op_tuple(&blocks->vregs[instr->result]));

	Janet *janet_args = janet_tuple_begin(3);
	janet_args[0] = jitted_op_tuple(instruction_arg(blocks, instr, 0));
	janet_args[1] = jitted_op_tuple(instruction_arg(blocks, instr, 1));
	janet_args[2] = jitted_op_tuple(instruction_arg(blocks, instr, 2));

	janet_table_put(janet_instr, janet_ckeywordv("args"), janet_wrap_tuple(janet_tuple_end(janet_args)));
	janet_bb_instrs[instr_i] = janet_wrap_struct(janet_table_to_struct(janet_instr));
      }
    }
    janet_bbs[block_i] = janet_wrap_tuple(janet_tuple_end(janet_bb_instrs));
  }
  return janet_wrap_tuple(janet_tuple_end(janet_bbs));
}

static int jitted_function_get(void *p, Janet key, Janet *out) {
  JittedFunction *jitted = p;
  if (janet_keyeq(key, "hir")) {
    *out = jitted_janet_hir(jitted);
    return 1;
  } else {
    janet_panic("unknown key for get");
  }
}

static Janet jitted_function_call(void *p, int32_t argc, Janet *argv) {
  JittedFunction *jitted = p;

  if (jitted->code == NULL) {
    // record signature
    jitted->signature_argc = argc;
    jitted->signature_arg_types = malloc(argc * sizeof(JanetType));
    for (int i = 0; i < argc; i++) {
      jitted->signature_arg_types[i] = janet_type(argv[i]);
    }
    type_flow(&jitted->method_blocks, argc, argv);
    register_allocate(&jitted->method_blocks);
    /* print_basic_blocks(&jitted->method_blocks); */
    compile(jitted);
    Janet res = ((JitFn)jitted->code)(argv, &jitted->ca);
    return res;
  }
  if (argc == jitted->signature_argc) {
    for (int i = 0; i < argc; i++) {
      if (janet_type(argv[i]) != jitted->signature_arg_types[i]) {
	return handle_mismatch(jitted, argc, argv);
	janet_panic("mismatching signature!");
      }
    }
    Janet res = ((JitFn)jitted->code)(argv, &jitted->ca);
    return res;
  } else {
    janet_panic("mismatching signature!");
  }
}

static const JanetAbstractType jitted_function_type = {
  "jittable-function",
  jitted_function_gc, // gc
  jitted_function_gcmark, // mark
  jitted_function_get, // get
  NULL, // put
  NULL, // marshal
  NULL, // unmarshal
  NULL, // tostring
  NULL, // compare
  NULL, // hash
  NULL, // next
  jitted_function_call, // call
  NULL, // length
  NULL, // bytes
  NULL  // gcperfthread
};

static Janet jit_jitable(int32_t argc, Janet *argv) {
  janet_arity(argc, 1, 2);
  JanetFunction *fn = janet_getfunction(argv, 0);
  if (fn->def->min_arity != fn->def->max_arity) {
    janet_panic("only fixed arity functions are supported");
  }
  char *mismatch_action = (char*)janet_optkeyword(argv, argc, 1, "fallback");

  JittedFunction *jitted =
    janet_abstract(&jitted_function_type, sizeof(JittedFunction));

  if (strcmp(mismatch_action, "fallback") == 0) {
    jitted->mismatch_behavior = MISMATCH_FALLBACK;
  } else if (strcmp(mismatch_action, "error") == 0) {
    jitted->mismatch_behavior = MISMATCH_ERROR;
  } else if (strcmp(mismatch_action, "recompile") == 0) {
    jitted->mismatch_behavior = MISMATCH_RECOMPILE;
  }

  jitted->ca = (CallArgs) {
    .count = 0,
    .capacity = 0,
    .argv = NULL
  };

  jitted->code_size = 0;
  jitted->code = NULL;
  jitted->fallback = fn;
  jitted->signature_argc = fn->def->min_arity;
  jitted->signature_arg_types = NULL;
  setup_method_blocks(&jitted->method_blocks, fn->def->bytecode_length);
  build_basic_blocks(jitted, fn);

  return janet_wrap_abstract(jitted);
}

static Janet jit_compiled(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  JittedFunction *jitted = janet_getabstract(argv, 0, &jitted_function_type);
  if (jitted->code == NULL) {
    return janet_wrap_false();
  } else {
    return janet_wrap_true();
  }
}

static const JanetReg cfuns[] = {
  {"jitable", jit_jitable, "(jit/jitable function &opt fallback-behavior)\n\nReturns a Jittable version of a function. Jit compilation happens on first call.\nYou can also specify a fallback behavior. the options are :fallback, :error and :recompile\n:fallback will call Janet when the signature does not match.\n:error will panic when the signature does not match. This is useful in dev and test environments to make sure you are getting benefit from your jitted methods.\n:recompile will compile again for the new argument types (This option is planned but not supported yet."},
  {"compiled?", jit_compiled, "(jit/compiled? function)\n\nReturns if the function successfully compiled."},
  {NULL, NULL, NULL}
};

JANET_MODULE_ENTRY(JanetTable *env) {
  janet_cfuns(env, "jit", cfuns);
  janet_register_abstract_type(&jitted_function_type);
}
