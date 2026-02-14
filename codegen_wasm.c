#include "chibicc.h"

static FILE *output_file;
static Obj *current_fn;
static int indent_level;
static int wasm_label_count;

__attribute__((format(printf, 1, 2)))
static void println(char *fmt, ...) {
  for (int i = 0; i < indent_level; i++)
    fprintf(output_file, "  ");
  va_list ap;
  va_start(ap, fmt);
  vfprintf(output_file, fmt, ap);
  va_end(ap);
  fprintf(output_file, "\n");
}

static void indent(void) { indent_level++; }
static void dedent(void) { indent_level--; }

static int wasm_count(void) {
  return wasm_label_count++;
}

// Map C type to wasm value type
static char *wasm_type(Type *ty) {
  if (!ty) return "i32";
  switch (ty->kind) {
  case TY_FLOAT: return "f32";
  case TY_DOUBLE:
  case TY_LDOUBLE: return "f64";
  case TY_LONG:
    if (ty->size == 8) return "i64";
    return "i32";
  default: return "i32";
  }
}

static bool is_wasm_i64(Type *ty) {
  return ty && ty->kind == TY_LONG && ty->size == 8;
}

static bool is_wasm_f32(Type *ty) {
  return ty && ty->kind == TY_FLOAT;
}

static bool is_wasm_f64(Type *ty) {
  return ty && (ty->kind == TY_DOUBLE || ty->kind == TY_LDOUBLE);
}

static bool is_wasm_float(Type *ty) {
  return is_wasm_f32(ty) || is_wasm_f64(ty);
}

// Get the effective wasm size for a type (pointers are always 4 bytes in wasm32)
static int wasm_size(Type *ty) {
  if (!ty) return 4;
  if (ty->kind == TY_PTR || ty->kind == TY_FUNC) return 4;
  if (ty->kind == TY_LONG) return 4; // treat long as i32 in wasm32
  return ty->size;
}

// Load from address on stack, based on type
static void wasm_load(Type *ty) {
  if (!ty) return;

  if (ty->kind == TY_ARRAY || ty->kind == TY_STRUCT ||
      ty->kind == TY_UNION || ty->kind == TY_FUNC) {
    // Arrays/structs/unions: address IS the value (pointer)
    return;
  }

  if (ty->kind == TY_FLOAT) {
    println("(f32.load)");
    return;
  }
  if (ty->kind == TY_DOUBLE || ty->kind == TY_LDOUBLE) {
    println("(f64.load)");
    return;
  }

  // Integer types (including pointers as i32)
  int sz = wasm_size(ty);
  switch (sz) {
  case 1:
    println(ty->is_unsigned ? "(i32.load8_u)" : "(i32.load8_s)");
    break;
  case 2:
    println(ty->is_unsigned ? "(i32.load16_u)" : "(i32.load16_s)");
    break;
  default:
    println("(i32.load)");
    break;
  }
}

// Store to address (addr is 2nd on stack, value is top)
// Stack: [... addr val] -> [...]
static void wasm_store(Type *ty) {
  if (!ty) return;

  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
    // TODO: memcpy for struct assignment
    println(";; TODO: struct store (size=%d)", ty->size);
    println("(drop)");
    println("(drop)");
    return;
  }

  if (ty->kind == TY_FLOAT) {
    println("(f32.store)");
    return;
  }
  if (ty->kind == TY_DOUBLE || ty->kind == TY_LDOUBLE) {
    println("(f64.store)");
    return;
  }

  int sz = wasm_size(ty);
  switch (sz) {
  case 1: println("(i32.store8)"); break;
  case 2: println("(i32.store16)"); break;
  default: println("(i32.store)"); break;
  }
}

static void gen_expr(Node *node);
static void gen_stmt(Node *node);
static void gen_addr(Node *node);

// Push the address of a node onto the wasm stack
static void gen_addr(Node *node) {
  switch (node->kind) {
  case ND_VAR:
    if (node->var->is_local) {
      // local address = $__bp + offset
      println("(i32.add (local.get $__bp) (i32.const %d))", node->var->offset);
    } else {
      // global variable: address in data segment
      println("(i32.const %d) ;; &%s", node->var->offset, node->var->name);
    }
    return;
  case ND_DEREF:
    gen_expr(node->lhs);
    return;
  case ND_COMMA:
    gen_expr(node->lhs);
    println("(drop)");
    gen_addr(node->rhs);
    return;
  case ND_MEMBER:
    gen_addr(node->lhs);
    println("(i32.const %d)", node->member->offset);
    println("(i32.add)");
    return;
  default:
    error_tok(node->tok, "not an lvalue (wasm gen_addr)");
  }
}

static void gen_expr(Node *node) {
  if (!node) return;

  switch (node->kind) {
  case ND_NULL_EXPR:
    println("(i32.const 0)");
    return;

  case ND_NUM:
    if (is_wasm_f32(node->ty)) {
      println("(f32.const %f)", (float)node->fval);
    } else if (is_wasm_f64(node->ty)) {
      println("(f64.const %f)", (double)node->fval);
    } else if (is_wasm_i64(node->ty)) {
      println("(i64.const %lld)", (long long)node->val);
    } else {
      println("(i32.const %d)", (int)node->val);
    }
    return;

  case ND_VAR:
    gen_addr(node);
    wasm_load(node->ty);
    return;

  case ND_MEMBER:
    gen_addr(node);
    wasm_load(node->ty);
    return;

  case ND_ADDR:
    gen_addr(node->lhs);
    return;

  case ND_DEREF:
    gen_expr(node->lhs);
    wasm_load(node->ty);
    return;

  case ND_NEG:
    if (is_wasm_f32(node->ty)) {
      gen_expr(node->lhs);
      println("(f32.neg)");
    } else if (is_wasm_f64(node->ty)) {
      gen_expr(node->lhs);
      println("(f64.neg)");
    } else {
      println("(i32.const 0)");
      gen_expr(node->lhs);
      println("(i32.sub)");
    }
    return;

  case ND_NOT:
    gen_expr(node->lhs);
    println("(i32.eqz)");
    return;

  case ND_BITNOT:
    gen_expr(node->lhs);
    println("(i32.const -1)");
    println("(i32.xor)");
    return;

  case ND_ASSIGN: {
    // Need: addr on stack, then value, then store
    // But also need to leave the value as result
    gen_addr(node->lhs);
    gen_expr(node->rhs);
    // Tee pattern: store to addr but keep value
    // Use a local temp
    if (is_wasm_float(node->ty)) {
      char *wt = wasm_type(node->ty);
      println("(local.set $__tmp_%s)", wt);
      println("(local.get $__tmp_%s)", wt);
      wasm_store(node->ty);
      println("(local.get $__tmp_%s)", wt);
    } else {
      println("(local.set $__tmp_i32)");
      println("(local.get $__tmp_i32)");
      wasm_store(node->ty);
      println("(local.get $__tmp_i32)");
    }
    return;
  }

  case ND_COMMA:
    gen_expr(node->lhs);
    println("(drop)");
    gen_expr(node->rhs);
    return;

  case ND_CAST: {
    gen_expr(node->lhs);
    Type *from = node->lhs->ty;
    Type *to = node->ty;
    if (!from || !to) return;

    // Same type family - may need truncation for smaller types
    if (!is_wasm_float(from) && !is_wasm_float(to) &&
        !is_wasm_i64(from) && !is_wasm_i64(to)) {
      // i32 -> i32, possibly with truncation
      if (to->kind == TY_BOOL) {
        println("(i32.const 0)");
        println("(i32.ne)");
      } else if (to->size == 1) {
        println(to->is_unsigned ? "(i32.const 255) (i32.and)" : "(i32.extend8_s)");
      } else if (to->size == 2) {
        println(to->is_unsigned ? "(i32.const 65535) (i32.and)" : "(i32.extend16_s)");
      }
      return;
    }

    // float conversions
    if (is_wasm_f32(from) && is_wasm_f64(to)) {
      println("(f64.promote_f32)");
    } else if (is_wasm_f64(from) && is_wasm_f32(to)) {
      println("(f32.demote_f64)");
    } else if (is_wasm_float(from) && !is_wasm_float(to)) {
      // float -> int
      if (is_wasm_f32(from))
        println(to->is_unsigned ? "(i32.trunc_f32_u)" : "(i32.trunc_f32_s)");
      else
        println(to->is_unsigned ? "(i32.trunc_f64_u)" : "(i32.trunc_f64_s)");
    } else if (!is_wasm_float(from) && is_wasm_float(to)) {
      // int -> float
      if (is_wasm_f32(to))
        println(from->is_unsigned ? "(f32.convert_i32_u)" : "(f32.convert_i32_s)");
      else
        println(from->is_unsigned ? "(f64.convert_i32_u)" : "(f64.convert_i32_s)");
    }
    return;
  }

  case ND_COND: {
    char *wt = wasm_type(node->ty);
    gen_expr(node->cond);
    println("(if (result %s)", wt);
    indent();
    println("(then");
    indent();
    gen_expr(node->then);
    dedent();
    println(")");
    println("(else");
    indent();
    if (node->els)
      gen_expr(node->els);
    else
      println("(%s.const 0)", wt);
    dedent();
    println(")");
    dedent();
    println(")");
    return;
  }

  case ND_LOGAND: {
    // Short-circuit: if lhs is 0, result is 0
    gen_expr(node->lhs);
    println("(if (result i32)");
    indent();
    println("(then");
    indent();
    gen_expr(node->rhs);
    println("(i32.const 0)");
    println("(i32.ne)");
    dedent();
    println(")");
    println("(else (i32.const 0))");
    dedent();
    println(")");
    return;
  }

  case ND_LOGOR: {
    gen_expr(node->lhs);
    println("(if (result i32)");
    indent();
    println("(then (i32.const 1))");
    println("(else");
    indent();
    gen_expr(node->rhs);
    println("(i32.const 0)");
    println("(i32.ne)");
    dedent();
    println(")");
    dedent();
    println(")");
    return;
  }

  case ND_FUNCALL: {
    // Push arguments
    int nargs = 0;
    for (Node *arg = node->args; arg; arg = arg->next) {
      gen_expr(arg);
      nargs++;
    }
    // Get function name
    if (node->lhs->kind == ND_VAR) {
      println("(call $%s)", node->lhs->var->name);
    } else {
      // Indirect call - skip for now
      println(";; TODO: indirect call");
      println("(drop)");
      println("(i32.const 0)");
    }
    return;
  }

  case ND_STMT_EXPR: {
    // Statement expression: ({ ... last_expr; })
    // Generate all but last as statements, last as expression
    Node *n = node->body;
    for (; n; n = n->next) {
      if (!n->next) {
        // Last statement - should be an expression statement
        if (n->kind == ND_EXPR_STMT) {
          gen_expr(n->lhs);
        } else {
          gen_stmt(n);
          println("(i32.const 0)");
        }
      } else {
        gen_stmt(n);
      }
    }
    return;
  }

  case ND_MEMZERO: {
    // Zero out a memory region
    int size = node->var->ty->size;
    println(";; memzero %s (%d bytes)", node->var->name, size);
    // Use memory.fill if available, otherwise loop
    println("(i32.add (local.get $__bp) (i32.const %d))", node->var->offset);
    println("(i32.const 0)");
    println("(i32.const %d)", size);
    println("(memory.fill)");
    return;
  }

  default:
    break;
  }

  // Binary operations
  if (node->lhs && node->rhs) {
    gen_expr(node->lhs);
    gen_expr(node->rhs);

    char *t = "i32";
    bool is_unsigned_int = node->lhs->ty && node->lhs->ty->is_unsigned;
    bool is_float = is_wasm_float(node->ty);
    if (is_float)
      t = wasm_type(node->ty);

    switch (node->kind) {
    case ND_ADD:
      println("(%s.add)", t);
      return;
    case ND_SUB:
      println("(%s.sub)", t);
      return;
    case ND_MUL:
      println("(%s.mul)", t);
      return;
    case ND_DIV:
      if (is_float)
        println("(%s.div)", t);
      else
        println(is_unsigned_int ? "(%s.div_u)" : "(%s.div_s)", t);
      return;
    case ND_MOD:
      println(is_unsigned_int ? "(%s.rem_u)" : "(%s.rem_s)", t);
      return;
    case ND_BITAND:
      println("(%s.and)", t);
      return;
    case ND_BITOR:
      println("(%s.or)", t);
      return;
    case ND_BITXOR:
      println("(%s.xor)", t);
      return;
    case ND_SHL:
      println("(%s.shl)", t);
      return;
    case ND_SHR:
      println(is_unsigned_int ? "(%s.shr_u)" : "(%s.shr_s)", t);
      return;
    case ND_EQ:
      println("(%s.eq)", t);
      return;
    case ND_NE:
      println("(%s.ne)", t);
      return;
    case ND_LT:
      if (is_float)
        println("(%s.lt)", t);
      else
        println(is_unsigned_int ? "(%s.lt_u)" : "(%s.lt_s)", t);
      return;
    case ND_LE:
      if (is_float)
        println("(%s.le)", t);
      else
        println(is_unsigned_int ? "(%s.le_u)" : "(%s.le_s)", t);
      return;
    default:
      break;
    }
  }

  error_tok(node->tok, "unsupported expression in wasm codegen (kind=%d)", node->kind);
}

static void gen_stmt(Node *node) {
  if (!node) return;

  switch (node->kind) {
  case ND_RETURN:
    if (node->lhs) {
      gen_expr(node->lhs);
    }
    println("(br $__return)");
    return;

  case ND_EXPR_STMT:
    gen_expr(node->lhs);
    // Drop the value since this is a statement
    if (node->lhs && node->lhs->ty && node->lhs->ty->kind != TY_VOID)
      println("(drop)");
    return;

  case ND_BLOCK:
    for (Node *n = node->body; n; n = n->next)
      gen_stmt(n);
    return;

  case ND_IF: {
    gen_expr(node->cond);
    println("(if");
    indent();
    println("(then");
    indent();
    gen_stmt(node->then);
    dedent();
    println(")");
    if (node->els) {
      println("(else");
      indent();
      gen_stmt(node->els);
      dedent();
      println(")");
    }
    dedent();
    println(")");
    return;
  }

  case ND_FOR: {
    // for (init; cond; inc) body
    // Maps to: init; block { loop { if (!cond) break; body; inc; br loop } }
    if (node->init)
      gen_stmt(node->init);

    println("(block $%s ;; break target", node->brk_label);
    indent();
    println("(loop $%s ;; continue target", node->cont_label);
    indent();

    if (node->cond) {
      gen_expr(node->cond);
      println("(i32.eqz)");
      println("(br_if $%s)", node->brk_label);
    }

    gen_stmt(node->then);

    if (node->inc) {
      gen_expr(node->inc);
      println("(drop)");
    }

    println("(br $%s)", node->cont_label);
    dedent();
    println(") ;; end loop");
    dedent();
    println(") ;; end block");
    return;
  }

  case ND_DO: {
    println("(block $%s ;; break target", node->brk_label);
    indent();
    println("(loop $%s ;; continue target", node->cont_label);
    indent();

    gen_stmt(node->then);

    gen_expr(node->cond);
    println("(br_if $%s)", node->cont_label);

    dedent();
    println(") ;; end loop");
    dedent();
    println(") ;; end block");
    return;
  }

  case ND_SWITCH: {
    gen_expr(node->cond);
    println("(local.set $__tmp_i32)");

    // Emit chained if/else for each case
    // First, wrap everything in a block for break
    println("(block $%s ;; break target", node->brk_label);
    indent();

    // Emit case tests as nested blocks with br
    // We'll use a simpler approach: chained if/else
    for (Node *n = node->case_next; n; n = n->case_next) {
      println("(local.get $__tmp_i32)");
      println("(i32.const %ld)", n->begin);
      println("(i32.eq)");
      println("(if (then");
      indent();
    }

    // Default or fall through
    if (node->default_case) {
      // Will be handled by the then body below
    }

    // Generate the body (which contains ND_CASE labels)
    gen_stmt(node->then);

    // Close all the if blocks
    for (Node *n = node->case_next; n; n = n->case_next) {
      dedent();
      println("))");
    }

    dedent();
    println(") ;; end break block");
    return;
  }

  case ND_CASE:
    // In our simplified model, just generate the body
    gen_stmt(node->lhs);
    return;

  case ND_GOTO:
    // TODO: goto support needs a state machine pattern
    println(";; TODO: goto %s", node->unique_label);
    return;

  case ND_LABEL:
    println(";; label: %s", node->label);
    gen_stmt(node->lhs);
    return;

  default:
    error_tok(node->tok, "unsupported statement in wasm codegen (kind=%d)", node->kind);
  }
}

// Assign memory offsets to local variables
static void assign_wasm_offsets(Obj *prog) {
  for (Obj *fn = prog; fn; fn = fn->next) {
    if (!fn->is_function)
      continue;

    int offset = 0;
    for (Obj *var = fn->locals; var; var = var->next) {
      offset = align_to(offset, var->ty->align > 0 ? var->ty->align : 1);
      var->offset = offset;
      offset += var->ty->size;
    }
    fn->stack_size = align_to(offset, 16);
  }
}

// Assign memory offsets to global variables
static int assign_global_offsets(Obj *prog) {
  int offset = 0; // globals start at address 0 in linear memory
  for (Obj *var = prog; var; var = var->next) {
    if (var->is_function)
      continue;
    offset = align_to(offset, var->ty->align > 0 ? var->ty->align : 1);
    var->offset = offset;
    offset += var->ty->size;
  }
  return align_to(offset, 16);
}

static void emit_data(Obj *prog) {
  for (Obj *var = prog; var; var = var->next) {
    if (var->is_function)
      continue;
    if (!var->init_data)
      continue;

    println(";; global: %s (offset=%d, size=%d)", var->name, var->offset, var->ty->size);
    // Emit as data segment
    fprintf(output_file, "  (data (i32.const %d) \"", var->offset);
    for (int i = 0; i < var->ty->size; i++) {
      unsigned char c = var->init_data[i];
      if (c >= 32 && c < 127 && c != '"' && c != '\\')
        fprintf(output_file, "%c", c);
      else
        fprintf(output_file, "\\%02x", c);
    }
    fprintf(output_file, "\")\n");
  }
}

static void emit_functions(Obj *prog) {
  for (Obj *fn = prog; fn; fn = fn->next) {
    if (!fn->is_function || !fn->is_definition)
      continue;
    if (!fn->is_live)
      continue;

    current_fn = fn;
    wasm_label_count = 0;

    // Function signature
    fprintf(output_file, "  (func $%s", fn->name);

    // Export main as _start
    if (strcmp(fn->name, "main") == 0)
      fprintf(output_file, " (export \"_start\")");

    // Parameters
    for (Obj *param = fn->params; param; param = param->next) {
      fprintf(output_file, " (param $p_%s %s)", param->name, wasm_type(param->ty));
    }

    // Return type
    Type *ret = fn->ty->return_ty;
    bool has_return = ret && ret->kind != TY_VOID;
    if (has_return) {
      fprintf(output_file, " (result %s)", wasm_type(ret));
    }
    fprintf(output_file, "\n");

    indent_level = 2;

    // Local variables
    println("(local $__bp i32)  ;; base pointer");
    println("(local $__tmp_i32 i32)");
    println("(local $__tmp_f32 f32)");
    println("(local $__tmp_f64 f64)");

    // Prologue: allocate stack frame
    println(";; prologue: allocate %d bytes", fn->stack_size);
    println("(global.set $__sp (i32.sub (global.get $__sp) (i32.const %d)))",
            fn->stack_size);
    println("(local.set $__bp (global.get $__sp))");

    // Copy parameters to their stack slots
    for (Obj *param = fn->params; param; param = param->next) {
      println(";; store param %s at bp+%d", param->name, param->offset);
      println("(i32.add (local.get $__bp) (i32.const %d))", param->offset);
      println("(local.get $p_%s)", param->name);
      wasm_store(param->ty);
    }

    // Body wrapped in a block for return jumps
    if (has_return) {
      println("(block $__return (result %s)", wasm_type(ret));
    } else {
      println("(block $__return");
    }
    indent();

    // Generate function body
    gen_stmt(fn->body);

    // Default return value (for main, return 0)
    if (has_return) {
      if (strcmp(fn->name, "main") == 0)
        println("(i32.const 0)");
      else
        println("(%s.const 0) ;; implicit return", wasm_type(ret));
    }

    dedent();
    println(") ;; end block $__return");

    // Epilogue: restore stack pointer
    println(";; epilogue");
    println("(global.set $__sp (i32.add (local.get $__bp) (i32.const %d)))",
            fn->stack_size);

    indent_level = 1;
    println(") ;; end func $%s", fn->name);
    fprintf(output_file, "\n");
  }
}

void codegen_wasm(Obj *prog, FILE *out) {
  output_file = out;
  indent_level = 0;

  // Assign memory layout
  int data_size = assign_global_offsets(prog);
  assign_wasm_offsets(prog);

  // Stack starts after global data, aligned to 64KB
  int stack_start = align_to(data_size + 1024, 65536);
  if (stack_start < 65536)
    stack_start = 65536;

  println("(module");
  indent();

  // Memory: 2 pages (128KB) - enough for basic programs
  println("(memory (export \"memory\") 2)");
  fprintf(output_file, "\n");

  // Stack pointer global
  println(";; Stack pointer (grows downward from %d)", stack_start);
  println("(global $__sp (mut i32) (i32.const %d))", stack_start);
  fprintf(output_file, "\n");

  // Data segments for initialized globals
  emit_data(prog);
  fprintf(output_file, "\n");

  // Functions
  emit_functions(prog);

  dedent();
  println(")");
}
