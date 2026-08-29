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
  /* OPERAND_PHYSICAL_REGISTER, */
  OPERAND_BASIC_BLOCK,
  OPERAND_SIGNED_IMM,
  OPERAND_UNSIGNED_IMM,
  OPERAND_JANET_IMM,
  OPERAND_PHI_SOURCE,
} OperandType;

typedef struct {
  size_t bb;
  size_t virtual_register;
  size_t slot;
} PhiSource;

typedef struct {
  OperandType type;
  union {
    size_t bb;
    size_t virtual_register;
    /* size_t physical_register; */
    uint64_t immus;
    int64_t imms;
  };
} Operand;

typedef enum {
  HIR_NOOP,
  HIR_ERROR,
  HIR_TYPECHECK,
  HIR_RETURN,
  HIR_ADD,
  HIR_SUB,
  HIR_MUL,
  HIR_DIV,
  HIR_DIV_FLOOR,
  HIR_MODULO,
  HIR_REAMINDER,
  HIR_AND,
  HIR_OR,
  HIR_XOR,
  HIR_NOT,
  HIR_LSHIFT,
  HIR_RSHIFT,
  HIR_RUSHIFT,
  HIR_JUMP,
  HIR_JUMP_IF,
  HIR_JUMP_IF_NOT,
  HIR_JUMP_IF_NIL,
  HIR_JUMP_IF_NOT_NIL,
  HIR_GREATER_THAN,
  HIR_LESS_THAN,
  HIR_EQUALS,
  HIR_COMPARE,
  HIR_LOAD,
  HIR_LOAD_ARG,
  HIR_PUSH,
  HIR_CALL,
  HIR_TAIL_CALL,
  HIR_IN,
  HIR_GET,
  HIR_PUT,
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
  HIR_PHI,
  HIR_PHI_PLACEHOLDER,
} InstructionType;

char *instruction_names[] = {
  "NOOP",
  "ERROR",
  "TYPECHECK",
  "RETURN",
  "ADD",
  "SUB",
  "MUL",
  "DIV",
  "DIV_FLOOR",
  "MODULO",
  "REAMINDER",
  "AND",
  "OR",
  "XOR",
  "NOT",
  "LSHIFT",
  "RSHIFT",
  "RUSHIFT",
  "JUMP",
  "JUMP_IF",
  "JUMP_IF_NOT",
  "JUMP_IF_NIL",
  "JUMP_IF_NOT_NIL",
  "GREATER_THAN",
  "LESS_THAN",
  "EQUALS",
  "COMPARE",
  "LOAD",
  "LOAD_ARG",
  "PUSH",
  "CALL",
  "TAIL_CALL",
  "IN",
  "GET",
  "PUT",
  "LENGTH",
  "MAKE_ARRAY",
  "MAKE_BUFFER",
  "MAKE_STRING",
  "MAKE_STRUCT",
  "MAKE_TABLE",
  "MAKE_TUPLE",
  "MAKE_BRACKET_TUPLE",
  "GREATER_THAN_EQUAL",
  "LESS_THAN_EQUAL",
  "NEXT",
  "NOT_EQUALS",
  "PHI",
  "PHI_PLACEHOLDER",
};

typedef struct {
  InstructionType type;
  Operand result;
  /* Operand branch_condition; */
  Operand operand1;
  Operand operand2;
  Operand operand3;

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
  size_t virtual_register_count;
} MethodBlocks;

typedef struct {
  size_t count;
  size_t capacity;
  uint8_t *data;

  size_t jump_index;
  int *jump_targets;
  int *jump_locations;
} CodeBuffer;


typedef struct {
  size_t count;
  size_t capacity;
  Janet *argv;
} CallArgs;

typedef struct {
  size_t code_size;
  void *code;
  JanetFunction *fallback;
  size_t signature_argc;
  JanetType *signature_arg_types;
  MethodBlocks method_blocks;
  CallArgs ca;
} JittedFunction;

void setup_method_blocks(MethodBlocks *blocks, size_t bytecode_length) {
  blocks->count = 0;
  blocks->capacity = 256;
  blocks->blocks = malloc(blocks->capacity * sizeof(BasicBlock));
  blocks->block_start_pcs = malloc(bytecode_length * sizeof(size_t));

  // TODO: this size is not accurate
  blocks->block_locations = malloc(bytecode_length * sizeof(size_t));
  blocks->virtual_register_count = 0;
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

  instruction->type = type;
  instruction->result.type = OPERAND_UNUSED;
  instruction->operand1.type = OPERAND_UNUSED;
  instruction->operand2.type = OPERAND_UNUSED;
  instruction->operand3.type = OPERAND_UNUSED;

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

  blocks->blocks[block_id].complete = false;
  blocks->blocks[block_id].start_pc = start_pc;
  blocks->blocks[block_id].count = 0;
  blocks->blocks[block_id].capacity = 256;
  blocks->blocks[block_id].instructions = malloc(blocks->blocks[block_id].capacity * sizeof(Instruction));

  blocks->blocks[block_id].slot_map = malloc(slotcount * sizeof(uint32_t));
  for (int i = 0; i < slotcount; i++) {
    blocks->blocks[block_id].slot_map[i] = UINT32_MAX;
  }

  blocks->blocks[block_id].slot_use = calloc(slotcount, sizeof(bool));
  blocks->blocks[block_id].slot_def = calloc(slotcount, sizeof(bool));
  blocks->blocks[block_id].live_in =  calloc(slotcount, sizeof(bool));
  blocks->blocks[block_id].live_out = calloc(slotcount, sizeof(bool));

  blocks->blocks[block_id].input_edge_count = 0;
  blocks->blocks[block_id].input_edge_capacity = 12;
  blocks->blocks[block_id].input_edges = malloc(blocks->blocks[block_id].input_edge_capacity * sizeof(Edge));
  blocks->blocks[block_id].output_edge_count = 0;
  blocks->blocks[block_id].output_edge_capacity = 12;
  blocks->blocks[block_id].output_edges = malloc(blocks->blocks[block_id].output_edge_capacity * sizeof(Edge));


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

static void compile_bb_bytecode(MethodBlocks *blocks, JanetFunction *fn, size_t block_id, uint32_t *parent_slot_map) {
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

	instruction->result.type = OPERAND_VIRTUAL_REGISTER;
	slot_map[slot_i] = blocks->virtual_register_count;
	instruction->result.virtual_register = blocks->virtual_register_count++;
      }
    }
  }

  blocks->blocks[block_id].slot_map = slot_map;

  blocks->blocks[block_id].complete = true;
  for (size_t index = blocks->blocks[block_id].start_pc; index < blocks->blocks[block_id].finish_pc; index++) {
    uint32_t instr = def->bytecode[index];
    switch (instr & 0x7F) {
    case JOP_NOOP:
      break;
    case JOP_ERROR: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_ERROR);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[AA];
      return;
    }
    case JOP_TYPECHECK: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_TYPECHECK);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[AA];
      instruction->operand2.type = OPERAND_UNSIGNED_IMM;
      instruction->operand2.immus = EE;
      break;
    }
    case JOP_RETURN: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_RETURN);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[DD];
      return;
    }
    case JOP_RETURN_NIL: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_RETURN);
      instruction->operand1.type = OPERAND_JANET_IMM;
      instruction->operand1.immus = janet_u64(janet_wrap_nil());
      return;
    }
    case JOP_ADD_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_ADD);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_SIGNED_IMM;
      instruction->operand2.imms = CS;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_ADD: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_ADD);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_SUBTRACT_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_SUB);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_SIGNED_IMM;
      instruction->operand2.imms = CS;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_SUBTRACT: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_SUB);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_MULTIPLY_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_MUL);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_SIGNED_IMM;
      instruction->operand2.imms = CS;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_MULTIPLY: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_MUL);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_DIVIDE_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_DIV);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_SIGNED_IMM;
      instruction->operand2.imms = CS;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_DIVIDE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_DIV);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_DIVIDE_FLOOR: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_DIV_FLOOR);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_MODULO: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_MODULO);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_REMAINDER: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_REAMINDER);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_BAND: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_AND);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_BOR: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_OR);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_BXOR: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_XOR);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_BNOT: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_NOT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[EE];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_SHIFT_LEFT: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LSHIFT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_SHIFT_LEFT_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LSHIFT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_SIGNED_IMM;
      instruction->operand2.imms = CS;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_SHIFT_RIGHT: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_RSHIFT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_SHIFT_RIGHT_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_RSHIFT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_SIGNED_IMM;
      instruction->operand2.imms = CS;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_SHIFT_RIGHT_UNSIGNED: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_RUSHIFT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_SHIFT_RIGHT_UNSIGNED_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_RUSHIFT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_UNSIGNED_IMM;
      instruction->operand2.immus = CC;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
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
      instruction->operand1.type = OPERAND_BASIC_BLOCK;
      instruction->operand1.bb =
        blocks->block_start_pcs[index + (((int32_t)instr) >> 8)];
      return;
    }
    case JOP_JUMP_IF: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_JUMP_IF);
      instruction->operand3.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand3.virtual_register = slot_map[AA];
      instruction->operand1.type = OPERAND_BASIC_BLOCK;
      instruction->operand1.bb =
        blocks->block_start_pcs[index + (((int32_t)instr) >> 16)];
      instruction->operand2.type = OPERAND_BASIC_BLOCK;
      instruction->operand2.bb = blocks->block_start_pcs[index + 1];
      return;
    }
    case JOP_JUMP_IF_NOT: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_JUMP_IF_NOT);
      instruction->operand3.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand3.virtual_register = slot_map[AA];
      instruction->operand1.type = OPERAND_BASIC_BLOCK;
      instruction->operand1.bb =
        blocks->block_start_pcs[index + (((int32_t)instr) >> 16)];
      instruction->operand2.type = OPERAND_BASIC_BLOCK;
      instruction->operand2.bb = blocks->block_start_pcs[index + 1];
      return;
    }
    case JOP_JUMP_IF_NIL: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_JUMP_IF_NIL);
      instruction->operand3.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand3.virtual_register = slot_map[AA];
      instruction->operand1.type = OPERAND_BASIC_BLOCK;
      instruction->operand1.bb =
        blocks->block_start_pcs[index + (((int32_t)instr) >> 16)];
      instruction->operand2.type = OPERAND_BASIC_BLOCK;
      instruction->operand2.bb = blocks->block_start_pcs[index + 1];
      return;
    }
    case JOP_JUMP_IF_NOT_NIL: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_JUMP_IF_NOT_NIL);
      instruction->operand3.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand3.virtual_register = slot_map[AA];
      instruction->operand1.type = OPERAND_BASIC_BLOCK;
      instruction->operand1.bb =
        blocks->block_start_pcs[index + (((int32_t)instr) >> 16)];
      instruction->operand2.type = OPERAND_BASIC_BLOCK;
      instruction->operand2.bb = blocks->block_start_pcs[index + 1];
      return;
    }
    case JOP_GREATER_THAN: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_GREATER_THAN);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_GREATER_THAN_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_GREATER_THAN);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_SIGNED_IMM;
      instruction->operand2.imms = CS;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_LESS_THAN: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LESS_THAN);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_LESS_THAN_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LESS_THAN);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_SIGNED_IMM;
      instruction->operand2.imms = CS;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_EQUALS: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_EQUALS);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_EQUALS_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_EQUALS);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_SIGNED_IMM;
      instruction->operand2.imms = CS;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_COMPARE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_COMPARE);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_LOAD_NIL: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LOAD);
      instruction->operand1.type = OPERAND_JANET_IMM;
      instruction->operand1.immus = janet_u64(janet_wrap_nil());
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[DD] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_LOAD_TRUE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LOAD);
      instruction->operand1.type = OPERAND_JANET_IMM;
      instruction->operand1.immus = janet_u64(janet_wrap_true());
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[DD] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_LOAD_FALSE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LOAD);
      instruction->operand1.type = OPERAND_JANET_IMM;
      instruction->operand1.immus = janet_u64(janet_wrap_false());
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[DD] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_LOAD_INTEGER: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LOAD);
      instruction->operand1.type = OPERAND_SIGNED_IMM;
      instruction->operand1.imms = ES;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_LOAD_CONSTANT: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LOAD);
      instruction->operand1.type = OPERAND_JANET_IMM;
      instruction->operand1.immus = janet_u64(def->constants[EE]);
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_LOAD_UPVALUE: {
      janet_panic("janet's upvalue opcode is not supported");
      break;
    }
    case JOP_LOAD_SELF: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LOAD);
      instruction->operand1.type = OPERAND_JANET_IMM;
      instruction->operand1.immus = janet_u64(janet_wrap_function(fn));
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[DD] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
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
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[DD];
      break;
    }
    case JOP_PUSH_2: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_PUSH);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[AA];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[EE];
      break;
    }
    case JOP_PUSH_3: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_PUSH);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[AA];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[BB];
      instruction->operand3.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand3.virtual_register = slot_map[CC];
      break;
    }
    case JOP_PUSH_ARRAY: {
      janet_panic("janet's push array is not yet supported");
      break;
    }
    case JOP_CALL: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_CALL);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[EE];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_TAILCALL: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_TAIL_CALL);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[DD];
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
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_GET: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_GET);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_PUT: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_PUT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[AA];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[BB];
      instruction->operand3.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand3.virtual_register = slot_map[CC];
      break;
    }
    case JOP_GET_INDEX: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_GET);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_UNSIGNED_IMM;
      instruction->operand2.immus = CC;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_PUT_INDEX: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_PUT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[AA];
      instruction->operand2.type = OPERAND_UNSIGNED_IMM;
      instruction->operand2.immus = CC;
      instruction->operand3.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand3.virtual_register = slot_map[BB];
      break;
    }
    case JOP_LENGTH: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LENGTH);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[EE];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_MAKE_ARRAY: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_MAKE_ARRAY);
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[DD] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_MAKE_BUFFER: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_MAKE_BUFFER);
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[DD] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_MAKE_STRING: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_MAKE_STRING);
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[DD] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_MAKE_STRUCT: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_MAKE_STRUCT);
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[DD] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_MAKE_TABLE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_MAKE_TABLE);
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[DD] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_MAKE_TUPLE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_MAKE_TUPLE);
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[DD] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_MAKE_BRACKET_TUPLE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_MAKE_BRACKET_TUPLE);
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[DD] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_GREATER_THAN_EQUAL: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_GREATER_THAN_EQUAL);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_LESS_THAN_EQUAL: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_LESS_THAN_EQUAL);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_NEXT: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_NEXT);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_NOT_EQUALS: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_NOT_EQUALS);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand2.virtual_register = slot_map[CC];
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
      break;
    }
    case JOP_NOT_EQUALS_IMMEDIATE: {
      Instruction *instruction = add_instruction(blocks, block_id, HIR_NOT_EQUALS);
      instruction->operand1.type = OPERAND_VIRTUAL_REGISTER;
      instruction->operand1.virtual_register = slot_map[BB];
      instruction->operand2.type = OPERAND_SIGNED_IMM;
      instruction->operand2.imms = CS;
      instruction->result.type = OPERAND_VIRTUAL_REGISTER;
      slot_map[AA] = blocks->virtual_register_count;
      instruction->result.virtual_register = blocks->virtual_register_count++;
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
  instruction->operand1.type = OPERAND_BASIC_BLOCK;
  instruction->operand1.bb = blocks->block_start_pcs[blocks->blocks[block_id].finish_pc];
}

static void rpo_order(MethodBlocks *blocks, size_t block_id, bool *visited, size_t *list, size_t *count) {
  if (visited[block_id]) { return; }
  visited[block_id] = true;
  for (size_t i = 0; i < blocks->blocks[block_id].output_edge_count; i++) {
    rpo_order(blocks, blocks->blocks[block_id].output_edges[i].basic_block_id, visited, list, count);
  }

  list[(*count)++] = block_id;
}

static void replace_virtual_register(MethodBlocks *blocks, size_t slotcount, size_t old, size_t new) {
  for (size_t block_i = 0; block_i < blocks->count; block_i++) {
    BasicBlock *block = &blocks->blocks[block_i];
    for (size_t slot_i = 0; slot_i < slotcount; slot_i++) {
      if (block->slot_map[slot_i] == old) {
	block->slot_map[slot_i] = new;
      }
    }

    for (size_t i = 0; i < block->count; i++) {
      Instruction *instr = &block->instructions[i];
      if (instr->operand1.type == OPERAND_VIRTUAL_REGISTER && instr->operand1.virtual_register == old) {
	instr->operand1.virtual_register = new;
      }

      if (instr->operand2.type == OPERAND_VIRTUAL_REGISTER && instr->operand2.virtual_register == old) {
	instr->operand2.virtual_register = new;
      }

      if (instr->operand3.type == OPERAND_VIRTUAL_REGISTER && instr->operand3.virtual_register == old) {
	instr->operand3.virtual_register = new;
      }

      switch (instr->type) {
      case HIR_PHI: {
	for (size_t i = 0; i < instr->phi_source_count; i++) {
	  PhiSource *source = &instr->phi_sources[i];
	  if (source->virtual_register == old) {
	    source->virtual_register = new;
	  }
	}
	break;
      }
      default:
	// noop
	break;
      }
    }
  }
}

void build_basic_blocks(MethodBlocks *blocks, JanetFunction *fn) {
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
    instruction->operand1.type = OPERAND_UNSIGNED_IMM;
    instruction->operand1.immus = i;
    instruction->result.type = OPERAND_VIRTUAL_REGISTER;
    slot_map[i] = blocks->virtual_register_count;
    instruction->result.virtual_register = blocks->virtual_register_count++;
  }

  // rpo order
  size_t *list = malloc(blocks->count * sizeof(size_t));
  bool *visited = calloc(blocks->count, sizeof(bool));
  size_t count = 0;
  rpo_order(blocks, 0, visited, list, &count);

  while (count > 0) {
    compile_bb_bytecode(blocks, fn, list[--count], slot_map);
  }

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
	    /* } */
	  }
	}

	if (instr->type == HIR_PHI) {
	  bool found_source = false;
	  bool needs_phi = false;
	  size_t replacement = 0;
	  for (size_t i = 0; i < instr->phi_source_count; i++) {
	    PhiSource *source = &instr->phi_sources[i];

	    if (source->virtual_register == instr->result.virtual_register) {
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
	    instr->type = HIR_NOOP;
	    changed = true;
	    replace_virtual_register(blocks, fn->def->slotcount, instr->result.virtual_register, replacement);
	  }
	}
      }
    }
  }

  // lower phis
  // TODO
}

void print_ops(Operand *op) {
  switch (op->type) {
  case OPERAND_VIRTUAL_REGISTER:
    printf("v%zu", op->virtual_register);
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

void print_basic_blocks(MethodBlocks *blocks) {
  // todo print method name etc.
  for (size_t block_i = 0; block_i < blocks->count; block_i++) {
    BasicBlock *bb = &blocks->blocks[block_i];
    printf("----block %zu : %zu instructions ----\n", block_i, bb->count);
    for (size_t instr_i = 0; instr_i < bb->count; instr_i++) {
      Instruction *instruction = &bb->instructions[instr_i];
      printf("%zu. ", instr_i);
      print_ops(&instruction->result);
	printf(" = ");
      printf("%s [", instruction_names[instruction->type]);
      if (instruction->type == HIR_PHI) {
	print_ops(&instruction->result);
	printf(", ");
	for (size_t i = 0; i < instruction->phi_source_count; i++) {
	  printf("(bb%zu: v%zu) ", instruction->phi_sources[i].bb, instruction->phi_sources[i].virtual_register);
	}
      } else {

	print_ops(&instruction->operand1);
	printf(", ");
	print_ops(&instruction->operand2);
	printf(", ");
	print_ops(&instruction->operand3);
      }
      printf("]\n");
    }
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

uint64_t jit_equals_fallback(Janet lhs, Janet rhs) {
  return janet_u64(janet_wrap_boolean(janet_equals(lhs, rhs)));
}

uint64_t jit_not_equals_fallback(Janet lhs, Janet rhs) {
  return janet_u64(janet_wrap_boolean(!janet_equals(lhs, rhs)));
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
  uint64_t result;
  if (janet_checktype(callee, JANET_FUNCTION)) {
    JanetFunction *func = janet_unwrap_function(callee);
    result = janet_u64(janet_call(func, call_args->count, call_args->argv));
  } else if (janet_checktype(callee, JANET_CFUNCTION)) {
    JanetCFunction func = janet_unwrap_cfunction(callee);
    int gc_lock = janet_gclock();
    result = janet_u64(func(call_args->count, call_args->argv));
    janet_gcunlock(gc_lock);
  } else if (janet_checktype(callee, JANET_ABSTRACT)) {
    JanetAbstract abstract = janet_unwrap_abstract(callee);
    const JanetAbstractType *at = janet_abstract_type(abstract);
    if (at->call != NULL) {
      int gc_lock = janet_gclock();
      result = janet_u64(at->call(abstract, call_args->count, call_args->argv));
      janet_gcunlock(gc_lock);
    } else {
      janet_panic("attempted to call uncallable abstract type");
    }
  } else if (janet_checktype(callee, JANET_KEYWORD)) {
    if (call_args->count == 0) {
      janet_panic("keyword argument on nil value");
    }

    Janet kwcallee = janet_get(call_args->argv[0], callee);
    if (janet_checktype(kwcallee, JANET_FUNCTION)) {
      JanetFunction *func = janet_unwrap_function(kwcallee);
      result = janet_u64(janet_call(func, call_args->count, call_args->argv));
    } else {
      janet_panicf("keyword function %p, %p is not callable", callee, kwcallee);
    }
  } else {
    janet_panic("attempted to call uncallable type");
  }
  call_args->count = 0;
  return result;
}


uint64_t jit_make_array(CallArgs *call_args) {
  Janet a = janet_wrap_array(janet_array_n(call_args->argv, call_args->count));
  call_args->count = 0;
  return janet_u64(a);
}

uint64_t jit_make_tuple(CallArgs *call_args) {
  JanetTuple t = janet_tuple_n(call_args->argv, call_args->count);
  Janet tup = janet_wrap_tuple(t);
  call_args->count = 0;
  return janet_u64(tup);
}

uint64_t jit_make_bracket_tuple(CallArgs *call_args) {
  JanetTuple t = janet_tuple_n(call_args->argv, call_args->count);
  janet_tuple_flag(t) |= JANET_TUPLE_FLAG_BRACKETCTOR;
  Janet tup = janet_wrap_tuple(t);
  call_args->count = 0;
  return janet_u64(tup);
}

uint64_t jit_make_buffer(CallArgs *call_args) {
  JanetBuffer *b = janet_buffer(call_args->count * 10);
  for (int i = 0; i < call_args->count; i++) {
    janet_to_string_b(b, call_args->argv[i]);
  }
  call_args->count = 0;
  return janet_u64(janet_wrap_buffer(b));
}

uint64_t jit_make_string(CallArgs *call_args) {
  JanetBuffer *b = janet_buffer(call_args->count * 10);
  for (int i = 0; i < call_args->count; i++) {
    janet_to_string_b(b, call_args->argv[i]);
  }
  call_args->count = 0;
  // TODO: this leaves a garbage buffer we can potentially skip
  return janet_u64(janet_stringv(b->data, b->count));
}

uint64_t jit_make_table(CallArgs *call_args) {
  if (call_args->count & 1) {
    janet_panicf("expected even number of arguments to table constructor, got %d", call_args->count);
  }
  JanetTable *tab = janet_table(call_args->count / 2);
  for (int i = 0; i < call_args->count; i += 2) {
    janet_table_put(tab, call_args->argv[i], call_args->argv[i + 1]);
  }
  call_args->count = 0;
  return janet_u64(janet_wrap_table(tab));
}

uint64_t jit_make_struct(CallArgs *call_args) {
  if (call_args->count & 1) {
    janet_panicf("expected even number of arguments to struct constructor, got %d", call_args->count);
  }
  JanetKV *st = janet_struct_begin(call_args->count / 2);
  for (int i = 0; i < call_args->count; i += 2) {
    janet_struct_put(st, call_args->argv[i], call_args->argv[i + 1]);
  }
  call_args->count = 0;
  return janet_u64(janet_wrap_struct(janet_struct_end(st)));
}

typedef Janet (*JitFn)(Janet *argv, CallArgs *call_args);

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

static void emit_stack_to_gpr(CodeBuffer *code, uint32_t dest, uint32_t source) {
  emit_byte(code, 0xF2);
  emit_byte(code, 0x48 | ((dest & 8) ? 0x04 : 0));
  emit_byte(code, 0x0F);
  emit_byte(code, 0x2C);
  emit_byte(code, 0x84 + ((dest & 7) << 3)); // store here
  emit_byte(code, 0x24);
  emit_u32(code, source * sizeof(Janet)); // from here
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

static void emit_operand_to_xmm(CodeBuffer *code, uint32_t destination, Operand *operand) {
  if (operand->type == OPERAND_VIRTUAL_REGISTER) {
    emit_stack_to_xmm(code, destination, operand->virtual_register);
    return;
  }

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

#define X86_ADD 0x58
#define X86_MUL 0x59
#define X86_SUB 0x5C
#define X86_DIV 0x5E

static void emit_binary_op(CodeBuffer *code, uint8_t op, uint32_t dest, uint32_t lhs, uint32_t rhs) {
  // op lhs, rhs
  emit_byte(code, 0xF2);
  emit_byte(code, 0x0F);
  emit_byte(code, op);
  emit_byte(code, 0xC0 + (lhs << 3) + rhs);
  if (dest != lhs) {
    // TODO: move. This isn't needed today since it will be spilled
  }
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
			    uint32_t destination) {
  emit_operand_to_xmm(code, reverse ? 1 : 0, lhs);
  emit_operand_to_xmm(code, reverse ? 0 : 1, rhs);
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

  emit_store_rax(code, destination);
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
  emit_byte(code, op); // and
  emit_byte(code, 0xC0 | ((rhs & 7) << 3) | (lhs & 7));
}

static void emit_not(CodeBuffer *code, uint32_t val) {
  emit_byte(code, 0x48 | ((val & 8) ? 0x01 : 0));
  emit_byte(code, 0xF7);
  emit_byte(code, 0xD0 | (val & 7));
}

static void emit_gpr_to_stack(CodeBuffer *code, uint32_t destination, uint32_t source) {
  // Convert the integer in RAX back to a Janet number in XMM0.
  emit_byte(code, 0xF2);
  emit_byte(code, 0x48 | ((source & 8) ? 0x01 : 0));
  emit_byte(code, 0x0F);
  emit_byte(code, 0x2A);
  emit_byte(code, 0xC0);
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
		       uint32_t destination,
		       uint32_t lhs,
		       Operand *rhs) {
  emit_stack_to_gpr(code, 0, lhs);

  if (rhs->type == OPERAND_VIRTUAL_REGISTER) {
    emit_stack_to_gpr(code, 1, rhs->virtual_register);
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
  emit_gpr_to_stack(code, destination, 0);
}

static void emit_compare(CodeBuffer *code,
			 Operand *lhs,
			 Operand *rhs,
			 uint32_t destination) {
  emit_operand_to_xmm(code, 0, lhs);
  emit_operand_to_xmm(code, 1, rhs);
  emit_ucomisd(code, 0, 1);

  // MOV and CMOV preserve the comparison flags set by UCOMISD.
  emit_imm_gpr(code, 0, janet_u64(janet_wrap_integer(0)));
  emit_imm_gpr(code, 1, janet_u64(janet_wrap_integer(-1)));
  emit_imm_gpr(code, 2, janet_u64(janet_wrap_integer(1)));
  emit_cmov(code, X86_CMOV_BELOW, 0, 1);
  emit_cmov(code, X86_CMOV_ABOVE, 0, 2);
  emit_cmov(code, X86_CMOV_PARITY, 0, 2);

  emit_store_rax(code, destination);
}

// used for first arg when jumping back to C
static int arg_loc[] = { 7, 6, 2, 1 };
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

static void emit_cfun_call(CodeBuffer *code, void *func) {
  emit_byte(code, 0x48);
  emit_byte(code, 0xB8);
  emit_u64(code, (uint64_t)(uintptr_t)func);
  emit_byte(code, 0xFF);
  emit_byte(code, 0xD0);
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

static void emit_mov(CodeBuffer *code, uint32_t v_dest, uint32_t v_source) {
  emit_stack_to_rax(code, v_source);
  emit_store_rax(code, v_dest);
}

static void emit_phi_moves(CodeBuffer *code, MethodBlocks *blocks, uint32_t bb_source, uint32_t bb_target) {
  BasicBlock *target_block = &blocks->blocks[bb_target];
  for (size_t pc = 0; pc < target_block->count; pc++) {
    if (target_block->instructions[pc].type == HIR_PHI) {
      for (size_t phi_i = 0; phi_i < target_block->instructions[pc].phi_source_count; phi_i++) {
	if (target_block->instructions[pc].phi_sources[phi_i].bb == bb_source) {
	  emit_mov(code, target_block->instructions[pc].result.virtual_register, target_block->instructions[pc].phi_sources[phi_i].virtual_register);
	}
      }
    }
  }
}

static void emit_jump_placeholder(CodeBuffer *code, int32_t pc, size_t target_bb) {
  code->jump_targets[code->jump_index] = target_bb;
  code->jump_locations[code->jump_index] = code->count;
  code->jump_index++;
  emit_u32(code, 0);
}

static void emit_binary_fallback(CodeBuffer *code, Instruction *instr, JitBinaryFallback fallback) {
  emit_stack_to_arg(code, 0, instr->operand1.virtual_register);
  if (instr->operand2.type == OPERAND_VIRTUAL_REGISTER) {
    emit_stack_to_arg(code, 1, instr->operand2.virtual_register);
  } else {
    emit_imm_to_arg(code, 1, operand_to_janet_bits(&instr->operand2));
  }
  emit_cfun_call(code, fallback);
  emit_store_ret(code, instr->result.virtual_register);
}


static void emit_unary_fallback(CodeBuffer *code, Instruction *instr, JitUnaryFallback fallback) {
  emit_stack_to_arg(code, 0, instr->operand1.virtual_register);
  emit_cfun_call(code, fallback);
  emit_store_ret(code, instr->result.virtual_register);
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
      emit_stack_to_arg(code, 0, instr->operand1.virtual_register);
      emit_cfun_call(code, janet_panicv);
      break;
    }
    case HIR_TYPECHECK: {
      emit_stack_to_arg(code, 0, instr->operand1.virtual_register);
      emit_imm_to_arg(code, 1, instr->operand2.immus);
      emit_cfun_call(code, jit_typecheck);
      break;
    }
    case HIR_RETURN: {
      if (instr->operand1.type == OPERAND_VIRTUAL_REGISTER) {
	emit_stack_to_rax(code, instr->operand1.virtual_register);
      } else {
	emit_imm_gpr(code, 0, instr->operand1.immus);
      }

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
      /* emit_stack_to_xmm(code, 0, instr->operand1.virtual_register); */
      /* emit_operand_to_xmm(code, 1, &instr->operand2); */
      /* emit_binary_op(code, X86_ADD, instr->result.virtual_register, 0, 1); */
      /* emit_xmm_to_stack(code, instr->result.virtual_register, 0); */
      emit_binary_fallback(code, instr, jit_add_fallback);
      break;
    }
    case HIR_SUB: {
      /* emit_stack_to_xmm(code, 0, instr->operand1.virtual_register); */
      /* emit_operand_to_xmm(code, 1, &instr->operand2); */
      /* emit_binary_op(code, X86_SUB, instr->result.virtual_register, 0, 1); */
      /* emit_xmm_to_stack(code, instr->result.virtual_register, 0); */
      emit_binary_fallback(code, instr, jit_sub_fallback);
      break;
    }
    case HIR_MUL: {
      /* emit_stack_to_xmm(code, 0, instr->operand1.virtual_register); */
      /* emit_operand_to_xmm(code, 1, &instr->operand2); */
      /* emit_binary_op(code, X86_MUL, instr->result.virtual_register, 0, 1); */
      /* emit_xmm_to_stack(code, instr->result.virtual_register, 0); */
      emit_binary_fallback(code, instr, jit_mul_fallback);
      break;
    }
    case HIR_DIV: {
      /* emit_stack_to_xmm(code, 0, instr->operand1.virtual_register); */
      /* emit_operand_to_xmm(code, 1, &instr->operand2); */
      /* emit_binary_op(code, X86_DIV, instr->result.virtual_register, 0, 1); */
      /* emit_xmm_to_stack(code, instr->result.virtual_register, 0); */
      emit_binary_fallback(code, instr, jit_div_fallback);
      break;
    }
    case HIR_DIV_FLOOR: {
      /* emit_stack_to_xmm(code, 0, instr->operand1.virtual_register); */
      /* emit_stack_to_xmm(code, 1, instr->operand2.virtual_register); */
      /* emit_binary_op(code, X86_DIV, instr->result.virtual_register, 0, 1); */
      /* emit_floor(code, 0); */
      /* emit_xmm_to_stack(code, instr->result.virtual_register, 0); */
      emit_binary_fallback(code, instr, jit_divf_fallback);
      break;
    }
    case HIR_MODULO: {
      /* emit_stack_to_xmm(code, 0, instr->operand1.virtual_register); */
      /* emit_stack_to_xmm(code, 1, instr->operand1.virtual_register); */
      /* emit_stack_to_xmm(code, 2, instr->operand2.virtual_register); */
      /* emit_binary_op(code, X86_DIV, instr->result.virtual_register, 1, 2); */
      /* emit_floor(code, 1); */
      /* emit_binary_op(code, X86_MUL, instr->result.virtual_register, 1, 2); */
      /* emit_binary_op(code, X86_SUB, instr->result.virtual_register, 0, 1); */
      /* emit_xmm_to_stack(code, instr->result.virtual_register, 0); */
      emit_binary_fallback(code, instr, jit_mod_fallback);
      break;
    }
    case HIR_REAMINDER: {
      /* emit_stack_to_xmm(code, 0, instr->operand1.virtual_register); */
      /* emit_stack_to_xmm(code, 1, instr->operand1.virtual_register); */
      /* emit_stack_to_xmm(code, 2, instr->operand2.virtual_register); */
      /* emit_binary_op(code, X86_DIV, instr->result.virtual_register, 1, 2); */
      /* emit_trunc(code, 1); */
      /* emit_binary_op(code, X86_MUL, instr->result.virtual_register, 1, 2); */
      /* emit_binary_op(code, X86_SUB, instr->result.virtual_register, 0, 1); */
      /* emit_xmm_to_stack(code, instr->result.virtual_register, 0); */
      emit_binary_fallback(code, instr, jit_rem_fallback);
      break;
    }
    case HIR_AND: {
      // Should do I do range checks here, or just say, "The jit rolls over"
      /* emit_stack_to_gpr(code, 0, instr->operand1.virtual_register); */
      /* emit_stack_to_gpr(code, 1, instr->operand2.virtual_register); */
      /* emit_gpr_op(code, X86_AND_GPR, 0, 1); */
      /* emit_gpr_to_stack(code, instr->result.virtual_register, 0); */
      emit_binary_fallback(code, instr, jit_band_fallback);
      break;
    }
    case HIR_OR: {
      /* emit_stack_to_gpr(code, 0, instr->operand1.virtual_register); */
      /* emit_stack_to_gpr(code, 1, instr->operand2.virtual_register); */
      /* emit_gpr_op(code, X86_OR_GPR, 0, 1); */
      /* emit_gpr_to_stack(code, instr->result.virtual_register, 0); */
      emit_binary_fallback(code, instr, jit_bor_fallback);
      break;
    }
    case HIR_XOR: {
      /* emit_stack_to_gpr(code, 0, instr->operand1.virtual_register); */
      /* emit_stack_to_gpr(code, 1, instr->operand2.virtual_register); */
      /* emit_gpr_op(code, X86_XOR_GPR, 0, 1); */
      /* emit_gpr_to_stack(code, instr->result.virtual_register, 0); */
      emit_binary_fallback(code, instr, jit_bxor_fallback);
      break;
    }
    case HIR_NOT: {
      /* emit_stack_to_gpr(code, 0, instr->operand1.virtual_register); */
      /* emit_not(code, 0); */
      /* emit_gpr_to_stack(code, instr->result.virtual_register, 0); */
      emit_unary_fallback(code, instr, jit_bnot_fallback);
      break;
    }
    case HIR_LSHIFT: {
      /* emit_shift(code, */
      /* 		 X86_SHIFT_LEFT, */
      /* 		 true, */
      /* 		 instr->result.virtual_register, */
      /* 		 instr->operand1.virtual_register, */
      /* 		 &instr->operand2); */
      emit_binary_fallback(code, instr, jit_blshift_fallback);
      break;
    }
    case HIR_RSHIFT: {
      /* emit_shift(code, */
      /* 		 X86_SHIFT_RIGHT_SIGNED, */
      /* 		 true, */
      /* 		 instr->result.virtual_register, */
      /* 		 instr->operand1.virtual_register, */
      /* 		 &instr->operand2); */
      emit_binary_fallback(code, instr, jit_brshift_fallback);
      break;
    }
    case HIR_RUSHIFT: {
      /* emit_shift(code, */
      /* 		 X86_SHIFT_RIGHT_UNSIGNED, */
      /* 		 false, */
      /* 		 instr->result.virtual_register, */
      /* 		 instr->operand1.virtual_register, */
      /* 		 &instr->operand2); */
      emit_binary_fallback(code, instr, jit_brushift_fallback);
      break;
    }
    case HIR_JUMP: {
      emit_phi_moves(code, blocks, block_id, instr->operand1.bb);
      emit_byte(code, 0xE9); // unconditional jump
      emit_jump_placeholder(code, code->count, instr->operand1.bb);
      break;
    }
    case HIR_JUMP_IF: {
      emit_stack_to_arg(code, 0, instr->operand3.virtual_register);
      emit_cfun_call(code, janet_truthy);
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
      emit_phi_moves(code, blocks, block_id, instr->operand1.bb);
      emit_byte(code, 0xE9); // unconditional jump
      emit_jump_placeholder(code, code->count, instr->operand1.bb);

      edit_u32(code, internal_jump_pc, code->count - (internal_jump_pc + 4));
      // phis for conditional false destination <A>
      emit_phi_moves(code, blocks, block_id, instr->operand2.bb);
      emit_byte(code, 0xE9); // unconditional jump
      emit_jump_placeholder(code, code->count, instr->operand2.bb);
      break;
    }
    case HIR_JUMP_IF_NOT: {
      // jump if conditional true to <A>
      emit_stack_to_arg(code, 0, instr->operand3.virtual_register);
      emit_cfun_call(code, janet_truthy);
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
      emit_phi_moves(code, blocks, block_id, instr->operand1.bb);
      emit_byte(code, 0xE9); // unconditional jump
      emit_jump_placeholder(code, code->count, instr->operand1.bb);

      edit_u32(code, internal_jump_pc, code->count - (internal_jump_pc + 4));
      // phis for conditional false destination <A>
      emit_phi_moves(code, blocks, block_id, instr->operand2.bb);
      emit_byte(code, 0xE9); // unconditional jump
      emit_jump_placeholder(code, code->count, instr->operand2.bb);
      break;
    }
    case HIR_JUMP_IF_NIL: {
      // jump if conditional true to <A>
      emit_stack_to_arg(code, 0, instr->operand3.virtual_register);
      emit_cfun_call(code, jit_nil);
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
      emit_phi_moves(code, blocks, block_id, instr->operand1.bb);
      emit_byte(code, 0xE9); // unconditional jump
      emit_jump_placeholder(code, code->count, instr->operand1.bb);

      edit_u32(code, internal_jump_pc, code->count - (internal_jump_pc + 4));
      // phis for conditional false destination <A>
      emit_phi_moves(code, blocks, block_id, instr->operand2.bb);
      emit_byte(code, 0xE9); // unconditional jump
      emit_jump_placeholder(code, code->count, instr->operand2.bb);
      break;
    }
    case HIR_JUMP_IF_NOT_NIL: {
      // jump if conditional true to <A>
      emit_stack_to_arg(code, 0, instr->operand3.virtual_register);
      emit_cfun_call(code, jit_nil);
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
      emit_phi_moves(code, blocks, block_id, instr->operand1.bb);
      emit_byte(code, 0xE9); // unconditional jump
      emit_jump_placeholder(code, code->count, instr->operand1.bb);

      edit_u32(code, internal_jump_pc, code->count - (internal_jump_pc + 4));
      // phis for conditional false destination <A>
      emit_phi_moves(code, blocks, block_id, instr->operand2.bb);
      emit_byte(code, 0xE9); // unconditional jump
      emit_jump_placeholder(code, code->count, instr->operand2.bb);
      break;
    }
    case HIR_GREATER_THAN: {
      emit_comparison(code,
		      &instr->operand1,
		      &instr->operand2,
		      false,
		      X86_CMOV_ABOVE,
		      -1,
		      instr->result.virtual_register);
      break;
    }
    case HIR_LESS_THAN: {
      emit_comparison(code,
		      &instr->operand1,
		      &instr->operand2,
		      true,
		      X86_CMOV_ABOVE,
		      -1,
		      instr->result.virtual_register);
      break;
    }
    case HIR_EQUALS: {
      /* emit_comparison(code, */
      /* 		      &instr->operand1, */
      /* 		      &instr->operand2, */
      /* 		      false, */
      /* 		      X86_CMOV_EQUAL, */
      /* 		      false, */
      /* 		      instr->result.virtual_register); */
      emit_binary_fallback(code, instr, jit_equals_fallback);
      break;
    }
    case HIR_COMPARE: {
      emit_compare(code,
		   &instr->operand1,
		   &instr->operand2,
		   instr->result.virtual_register);
      break;
    }
    case HIR_LOAD: {
      emit_imm_gpr(code, 0, operand_to_janet_bits(&instr->operand1));
      emit_store_rax(code, instr->result.virtual_register);
      break;
    }
    case HIR_LOAD_ARG: {
      emit_arg_to_stack(code, instr->operand1.immus, instr->result.virtual_register);
      break;
    }
    case HIR_PUSH: {
      emit_non_janet_to_arg(code, 0, call_args_loc);
      int argc = 0;
      if (instr->operand1.type == OPERAND_VIRTUAL_REGISTER) {
	argc++;
	emit_stack_to_arg(code, 1, instr->operand1.virtual_register);
      }
      if (instr->operand2.type == OPERAND_VIRTUAL_REGISTER) {
	emit_stack_to_arg(code, 2, instr->operand2.virtual_register);
	argc++;
      }
      if (instr->operand3.type == OPERAND_VIRTUAL_REGISTER) {
	emit_stack_to_arg(code, 3, instr->operand3.virtual_register);
	argc++;
      }

      switch (argc) {
      case 1:
	emit_cfun_call(code, jit_push);
	break;
      case 2:
	emit_cfun_call(code, jit_push_2);
	break;
      case 3:
	emit_cfun_call(code, jit_push_3);
	break;
      }
      break;
    }
    case HIR_CALL: {
      emit_stack_to_arg(code, 0, instr->operand1.virtual_register);
      emit_non_janet_to_arg(code, 1, call_args_loc);
      emit_cfun_call(code, jit_call);
      emit_store_ret(code, instr->result.virtual_register);
      break;
    }
    case HIR_TAIL_CALL: {
      emit_stack_to_arg(code, 0, instr->operand1.virtual_register);
      emit_non_janet_to_arg(code, 1, call_args_loc);
      emit_cfun_call(code, jit_call);
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
      emit_stack_to_arg(code, 0, instr->operand1.virtual_register);
      if (instr->operand2.type == OPERAND_VIRTUAL_REGISTER) {
	emit_stack_to_arg(code, 1, instr->operand2.virtual_register);
      } else {
	emit_imm_to_arg(code, 1, instr->operand2.imms);
      }
      emit_cfun_call(code, janet_in);
      emit_store_ret(code, instr->result.virtual_register);
      break;
    }
    case HIR_GET: {
      emit_stack_to_arg(code, 0, instr->operand1.virtual_register);
      if (instr->operand2.type == OPERAND_VIRTUAL_REGISTER) {
	emit_stack_to_arg(code, 1, instr->operand2.virtual_register);
	emit_cfun_call(code, janet_get);
      } else {
	emit_imm_to_arg(code, 1, instr->operand2.immus);
	emit_cfun_call(code, janet_getindex);
      }
      emit_store_ret(code, instr->result.virtual_register);
      break;
    }
    case HIR_PUT: {
      emit_stack_to_arg(code, 0, instr->operand1.virtual_register);
      if (instr->operand2.type == OPERAND_VIRTUAL_REGISTER) {
	emit_stack_to_arg(code, 1, instr->operand2.virtual_register);
      } else {
	emit_imm_to_arg(code, 1, operand_to_janet_bits(&instr->operand2));
      }
      emit_stack_to_arg(code, 2, instr->operand3.virtual_register);
      emit_cfun_call(code, janet_put);
      break;
    }
    case HIR_LENGTH: {
      emit_stack_to_arg(code, 0, instr->operand1.virtual_register);
      emit_cfun_call(code, janet_lengthv);
      emit_store_ret(code, instr->result.virtual_register);
      break;
    }
    case HIR_MAKE_ARRAY:
      emit_non_janet_to_arg(code, 0, call_args_loc);
      emit_cfun_call(code, jit_make_array);
      emit_store_ret(code, instr->result.virtual_register);
      break;
    case HIR_MAKE_TUPLE:
      emit_non_janet_to_arg(code, 0, call_args_loc);
      emit_cfun_call(code, jit_make_tuple);
      emit_store_ret(code, instr->result.virtual_register);
      break;
    case HIR_MAKE_BRACKET_TUPLE:
      emit_non_janet_to_arg(code, 0, call_args_loc);
      emit_cfun_call(code, jit_make_bracket_tuple);
      emit_store_ret(code, instr->result.virtual_register);
      break;
    case HIR_MAKE_BUFFER:
      emit_non_janet_to_arg(code, 0, call_args_loc);
      emit_cfun_call(code, jit_make_buffer);
      emit_store_ret(code, instr->result.virtual_register);
      break;
    case HIR_MAKE_STRING:
      emit_non_janet_to_arg(code, 0, call_args_loc);
      emit_cfun_call(code, jit_make_string);
      emit_store_ret(code, instr->result.virtual_register);
      break;
    case HIR_MAKE_STRUCT:
      emit_non_janet_to_arg(code, 0, call_args_loc);
      emit_cfun_call(code, jit_make_struct);
      emit_store_ret(code, instr->result.virtual_register);
      break;
    case HIR_MAKE_TABLE:
      emit_non_janet_to_arg(code, 0, call_args_loc);
      emit_cfun_call(code, jit_make_table);
      emit_store_ret(code, instr->result.virtual_register);
      break;
    case HIR_GREATER_THAN_EQUAL: {
      emit_comparison(code,
		      &instr->operand1,
		      &instr->operand2,
		      false,
		      X86_CMOV_ABOVE_EQUAL,
		      -1,
		      instr->result.virtual_register);
      break;
    }
    case HIR_LESS_THAN_EQUAL: {
      emit_comparison(code,
		      &instr->operand1,
		      &instr->operand2,
		      true,
		      X86_CMOV_ABOVE_EQUAL,
		      -1,
		      instr->result.virtual_register);
      break;
    }
    case HIR_NEXT: {
      emit_stack_to_arg(code, 0, instr->operand1.virtual_register);
      emit_stack_to_arg(code, 1, instr->operand2.virtual_register);
      emit_cfun_call(code, janet_next);
      emit_store_ret(code, instr->result.virtual_register);
      break;
    }
    case HIR_NOT_EQUALS: {
      /* emit_comparison(code, */
      /* 		      &instr->operand1, */
      /* 		      &instr->operand2, */
      /* 		      false, */
      /* 		      X86_CMOV_NOT_EQUAL, */
      /* 		      true, */
      /* 		      instr->result.virtual_register); */
      emit_binary_fallback(code, instr, jit_not_equals_fallback);
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
    .jump_targets = malloc(256 * sizeof(uint32_t)),
    .jump_locations = malloc(256 * sizeof(uint32_t)),
  };

  // stack size rounded to 16
  uint32_t stack_size =
    ((jitted->method_blocks.virtual_register_count * sizeof(Janet) + sizeof(CallArgs*)) + 7u & ~15u) + 8u;

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

static Janet jitted_function_call(void *p, int32_t argc, Janet *argv) {
  JittedFunction *jitted = p;

  /* for (int i = 0; i < argc; i++) { */
  /*   if (!janet_checktype(argv[0], JANET_NUMBER)) { */
  /*     janet_panicf("all args must be numbers (for now)"); */
  /*   } */
  /* } */

  if (jitted->code == NULL) {
    compile(jitted);
  }

  Janet res = ((JitFn)jitted->code)(argv, &jitted->ca);

  return res;
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
  JanetFunction *fn = janet_getfunction(argv, 0);
  if (fn->def->min_arity != fn->def->max_arity) {
    janet_panic("only fixed arity functions are supported");
  }

  JittedFunction *jitted =
    janet_abstract(&jitted_function_type, sizeof(JittedFunction));

  jitted->ca = (CallArgs) {
    .count = 0,
    .capacity = 0,
    argv = NULL
  };

  jitted->code_size = 0;
  jitted->code = NULL;
  jitted->fallback = fn;
  jitted->signature_argc = fn->def->min_arity;
  jitted->signature_arg_types = NULL;
  setup_method_blocks(&jitted->method_blocks, fn->def->bytecode_length);
  build_basic_blocks(&jitted->method_blocks, fn);
  /* print_basic_blocks(&jitted->method_blocks); */

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
  {"jitable", jit_jitable, "(jit/jitable function)\n\nReturns a Jittable version of a function. Jit compilation happens on first call."},
  {"compiled?", jit_compiled, "(jit/compiled? function)\n\nReturns if the function successfully compiled."},
  {NULL, NULL, NULL}
};

JANET_MODULE_ENTRY(JanetTable *env) {
  janet_cfuns(env, "jit", cfuns);
  janet_register_abstract_type(&jitted_function_type);
}
