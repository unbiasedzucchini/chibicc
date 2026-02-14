#include "chibicc.h"
#include <stdio.h>
#include <string.h>

// Maximum recursion depth for AST dumping
#define MAX_DEPTH 20

//
// JSON helper utilities
//

static void json_print_escaped(FILE *out, const char *s, int len) {
  fputc('"', out);
  for (int i = 0; i < len; i++) {
    unsigned char c = s[i];
    switch (c) {
    case '"': fputs("\\\"", out); break;
    case '\\': fputs("\\\\", out); break;
    case '\n': fputs("\\n", out); break;
    case '\r': fputs("\\r", out); break;
    case '\t': fputs("\\t", out); break;
    case '\0': fputs("\\u0000", out); break;
    default:
      if (c < 0x20)
        fprintf(out, "\\u%04x", c);
      else
        fputc(c, out);
    }
  }
  fputc('"', out);
}

static void json_print_str(FILE *out, const char *s) {
  if (!s) {
    fputs("null", out);
    return;
  }
  json_print_escaped(out, s, strlen(s));
}

//
// Token kind to string
//

static const char *token_kind_name(TokenKind kind) {
  switch (kind) {
  case TK_IDENT:   return "TK_IDENT";
  case TK_PUNCT:   return "TK_PUNCT";
  case TK_KEYWORD: return "TK_KEYWORD";
  case TK_STR:     return "TK_STR";
  case TK_NUM:     return "TK_NUM";
  case TK_PP_NUM:  return "TK_PP_NUM";
  case TK_EOF:     return "TK_EOF";
  }
  return "TK_UNKNOWN";
}

//
// Node kind to string
//

static const char *node_kind_name(NodeKind kind) {
  switch (kind) {
  case ND_NULL_EXPR: return "ND_NULL_EXPR";
  case ND_ADD:       return "ND_ADD";
  case ND_SUB:       return "ND_SUB";
  case ND_MUL:       return "ND_MUL";
  case ND_DIV:       return "ND_DIV";
  case ND_NEG:       return "ND_NEG";
  case ND_MOD:       return "ND_MOD";
  case ND_BITAND:    return "ND_BITAND";
  case ND_BITOR:     return "ND_BITOR";
  case ND_BITXOR:    return "ND_BITXOR";
  case ND_SHL:       return "ND_SHL";
  case ND_SHR:       return "ND_SHR";
  case ND_EQ:        return "ND_EQ";
  case ND_NE:        return "ND_NE";
  case ND_LT:        return "ND_LT";
  case ND_LE:        return "ND_LE";
  case ND_ASSIGN:    return "ND_ASSIGN";
  case ND_COND:      return "ND_COND";
  case ND_COMMA:     return "ND_COMMA";
  case ND_MEMBER:    return "ND_MEMBER";
  case ND_ADDR:      return "ND_ADDR";
  case ND_DEREF:     return "ND_DEREF";
  case ND_NOT:       return "ND_NOT";
  case ND_BITNOT:    return "ND_BITNOT";
  case ND_LOGAND:    return "ND_LOGAND";
  case ND_LOGOR:     return "ND_LOGOR";
  case ND_RETURN:    return "ND_RETURN";
  case ND_IF:        return "ND_IF";
  case ND_FOR:       return "ND_FOR";
  case ND_DO:        return "ND_DO";
  case ND_SWITCH:    return "ND_SWITCH";
  case ND_CASE:      return "ND_CASE";
  case ND_BLOCK:     return "ND_BLOCK";
  case ND_GOTO:      return "ND_GOTO";
  case ND_GOTO_EXPR: return "ND_GOTO_EXPR";
  case ND_LABEL:     return "ND_LABEL";
  case ND_LABEL_VAL: return "ND_LABEL_VAL";
  case ND_FUNCALL:   return "ND_FUNCALL";
  case ND_EXPR_STMT: return "ND_EXPR_STMT";
  case ND_STMT_EXPR: return "ND_STMT_EXPR";
  case ND_VAR:       return "ND_VAR";
  case ND_VLA_PTR:   return "ND_VLA_PTR";
  case ND_NUM:       return "ND_NUM";
  case ND_CAST:      return "ND_CAST";
  case ND_MEMZERO:   return "ND_MEMZERO";
  case ND_ASM:       return "ND_ASM";
  case ND_CAS:       return "ND_CAS";
  case ND_EXCH:      return "ND_EXCH";
  }
  return "ND_UNKNOWN";
}

//
// Type to string
//

// Build a human-readable type string (e.g. "int", "int *", "char [10]", etc.)
// Returns a dynamically allocated string.
static char *type_to_str(Type *ty) {
  if (!ty)
    return strdup("(null)");

  switch (ty->kind) {
  case TY_VOID:    return strdup(ty->is_unsigned ? "unsigned void" : "void");
  case TY_BOOL:    return strdup("_Bool");
  case TY_CHAR:    return strdup(ty->is_unsigned ? "unsigned char" : "char");
  case TY_SHORT:   return strdup(ty->is_unsigned ? "unsigned short" : "short");
  case TY_INT:     return strdup(ty->is_unsigned ? "unsigned int" : "int");
  case TY_LONG:    return strdup(ty->is_unsigned ? "unsigned long" : "long");
  case TY_FLOAT:   return strdup("float");
  case TY_DOUBLE:  return strdup("double");
  case TY_LDOUBLE: return strdup("long double");
  case TY_ENUM:    return strdup("enum");
  case TY_STRUCT: {
    char buf[64];
    snprintf(buf, sizeof(buf), "struct(%d)", ty->size);
    return strdup(buf);
  }
  case TY_UNION: {
    char buf[64];
    snprintf(buf, sizeof(buf), "union(%d)", ty->size);
    return strdup(buf);
  }
  case TY_PTR: {
    char *base = type_to_str(ty->base);
    char *buf = calloc(1, strlen(base) + 4);
    sprintf(buf, "%s *", base);
    free(base);
    return buf;
  }
  case TY_ARRAY: {
    char *base = type_to_str(ty->base);
    char *buf = calloc(1, strlen(base) + 20);
    sprintf(buf, "%s[%d]", base, ty->array_len);
    free(base);
    return buf;
  }
  case TY_VLA: {
    char *base = type_to_str(ty->base);
    char *buf = calloc(1, strlen(base) + 8);
    sprintf(buf, "%s[*]", base);
    free(base);
    return buf;
  }
  case TY_FUNC: {
    char *ret = type_to_str(ty->return_ty);
    // Just show "ret_type (*)(...)"
    char *buf = calloc(1, strlen(ret) + 16);
    sprintf(buf, "%s (*)()", ret);
    free(ret);
    return buf;
  }
  }
  return strdup("unknown");
}

//
// --dump-tokens
//

void dump_tokens(Token *tok) {
  printf("[\n");
  bool first = true;
  for (Token *t = tok; t; t = t->next) {
    if (t->kind == TK_EOF)
      break;
    if (!first)
      printf(",\n");
    first = false;

    printf("  {\"kind\":");
    json_print_str(stdout, token_kind_name(t->kind));
    printf(",\"text\":");
    json_print_escaped(stdout, t->loc, t->len);
    printf(",\"line\":%d", t->line_no);
    printf(",\"file\":");
    json_print_str(stdout, t->filename);

    // For numeric tokens, include the value
    if (t->kind == TK_NUM) {
      if (t->ty && (t->ty->kind == TY_FLOAT || t->ty->kind == TY_DOUBLE || t->ty->kind == TY_LDOUBLE))
        printf(",\"fval\":%Lg", t->fval);
      else
        printf(",\"val\":%ld", (long)t->val);
    }

    printf("}");
  }
  printf("\n]\n");
}

//
// --dump-ast
//

static void dump_node(FILE *out, Node *node, int depth);

static void dump_node_list(FILE *out, const char *key, Node *node, int depth) {
  fprintf(out, ",\"%s\":[", key);
  bool first = true;
  for (Node *n = node; n; n = n->next) {
    if (!first) fputc(',', out);
    first = false;
    dump_node(out, n, depth);
  }
  fputc(']', out);
}

static void dump_node_field(FILE *out, const char *key, Node *node, int depth) {
  fprintf(out, ",\"%s\":", key);
  if (node)
    dump_node(out, node, depth);
  else
    fputs("null", out);
}

static void dump_node(FILE *out, Node *node, int depth) {
  if (!node) {
    fputs("null", out);
    return;
  }

  if (depth > MAX_DEPTH) {
    fputs("{\"kind\":\"...(truncated)\"}", out);
    return;
  }

  fputs("{\"kind\":", out);
  json_print_str(out, node_kind_name(node->kind));

  // Type
  if (node->ty) {
    char *ts = type_to_str(node->ty);
    fprintf(out, ",\"type\":");
    json_print_str(out, ts);
    free(ts);
  }

  // Token location
  if (node->tok) {
    fprintf(out, ",\"line\":%d", node->tok->line_no);
  }

  switch (node->kind) {
  case ND_NUM:
    if (node->ty && (node->ty->kind == TY_FLOAT || node->ty->kind == TY_DOUBLE || node->ty->kind == TY_LDOUBLE))
      fprintf(out, ",\"fval\":%Lg", node->fval);
    else
      fprintf(out, ",\"val\":%ld", (long)node->val);
    break;

  case ND_VAR:
    if (node->var) {
      fprintf(out, ",\"name\":");
      json_print_str(out, node->var->name);
    }
    break;

  case ND_FUNCALL:
    dump_node_field(out, "func", node->lhs, depth + 1);
    dump_node_list(out, "args", node->args, depth + 1);
    break;

  case ND_MEMBER:
    dump_node_field(out, "lhs", node->lhs, depth + 1);
    if (node->member && node->member->name) {
      fprintf(out, ",\"member\":");
      json_print_escaped(out, node->member->name->loc, node->member->name->len);
    }
    break;

  case ND_IF:
    dump_node_field(out, "cond", node->cond, depth + 1);
    dump_node_field(out, "then", node->then, depth + 1);
    if (node->els)
      dump_node_field(out, "els", node->els, depth + 1);
    break;

  case ND_FOR:
    if (node->init)
      dump_node_field(out, "init", node->init, depth + 1);
    if (node->cond)
      dump_node_field(out, "cond", node->cond, depth + 1);
    if (node->inc)
      dump_node_field(out, "inc", node->inc, depth + 1);
    dump_node_field(out, "then", node->then, depth + 1);
    break;

  case ND_DO:
    dump_node_field(out, "body", node->then, depth + 1);
    dump_node_field(out, "cond", node->cond, depth + 1);
    break;

  case ND_SWITCH:
    dump_node_field(out, "cond", node->cond, depth + 1);
    dump_node_field(out, "then", node->then, depth + 1);
    break;

  case ND_CASE:
    fprintf(out, ",\"begin\":%ld,\"end\":%ld", node->begin, node->end);
    dump_node_field(out, "body", node->lhs, depth + 1);
    break;

  case ND_BLOCK:
  case ND_STMT_EXPR:
    dump_node_list(out, "body", node->body, depth + 1);
    break;

  case ND_RETURN:
  case ND_EXPR_STMT:
  case ND_NEG:
  case ND_ADDR:
  case ND_DEREF:
  case ND_NOT:
  case ND_BITNOT:
  case ND_CAST:
    if (node->lhs)
      dump_node_field(out, "lhs", node->lhs, depth + 1);
    break;

  case ND_GOTO:
    if (node->label) {
      fprintf(out, ",\"label\":");
      json_print_str(out, node->label);
    }
    break;

  case ND_GOTO_EXPR:
    dump_node_field(out, "expr", node->lhs, depth + 1);
    break;

  case ND_LABEL:
    if (node->label) {
      fprintf(out, ",\"label\":");
      json_print_str(out, node->label);
    }
    dump_node_field(out, "body", node->lhs, depth + 1);
    break;

  case ND_LABEL_VAL:
    if (node->label) {
      fprintf(out, ",\"label\":");
      json_print_str(out, node->label);
    }
    break;

  case ND_ASM:
    if (node->asm_str) {
      fprintf(out, ",\"asm\":");
      json_print_str(out, node->asm_str);
    }
    break;

  case ND_CAS:
    dump_node_field(out, "addr", node->cas_addr, depth + 1);
    dump_node_field(out, "old", node->cas_old, depth + 1);
    dump_node_field(out, "new", node->cas_new, depth + 1);
    break;

  case ND_EXCH:
    dump_node_field(out, "lhs", node->lhs, depth + 1);
    dump_node_field(out, "rhs", node->rhs, depth + 1);
    break;

  case ND_COND:
    dump_node_field(out, "cond", node->cond, depth + 1);
    dump_node_field(out, "then", node->then, depth + 1);
    dump_node_field(out, "els", node->els, depth + 1);
    break;

  case ND_MEMZERO:
    if (node->var) {
      fprintf(out, ",\"name\":");
      json_print_str(out, node->var->name);
    }
    break;

  case ND_VLA_PTR:
    if (node->var) {
      fprintf(out, ",\"name\":");
      json_print_str(out, node->var->name);
    }
    break;

  default:
    // Binary ops: ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_MOD, ND_BITAND,
    // ND_BITOR, ND_BITXOR, ND_SHL, ND_SHR, ND_EQ, ND_NE, ND_LT, ND_LE,
    // ND_ASSIGN, ND_COMMA, ND_LOGAND, ND_LOGOR
    if (node->lhs)
      dump_node_field(out, "lhs", node->lhs, depth + 1);
    if (node->rhs)
      dump_node_field(out, "rhs", node->rhs, depth + 1);
    break;
  }

  fputc('}', out);
}

static void dump_type_json(FILE *out, Type *ty) {
  char *ts = type_to_str(ty);
  json_print_str(out, ts);
  free(ts);
}

void dump_ast(Obj *prog) {
  FILE *out = stdout;
  fputs("{\"globals\":[\n", out);

  bool first = true;
  for (Obj *obj = prog; obj; obj = obj->next) {
    if (!first) fputs(",\n", out);
    first = false;

    fputs("  {", out);
    fprintf(out, "\"name\":");
    json_print_str(out, obj->name);

    fprintf(out, ",\"is_function\":%s", obj->is_function ? "true" : "false");
    fprintf(out, ",\"is_definition\":%s", obj->is_definition ? "true" : "false");
    fprintf(out, ",\"is_static\":%s", obj->is_static ? "true" : "false");

    if (obj->ty) {
      fprintf(out, ",\"type\":");
      dump_type_json(out, obj->ty);
    }

    if (obj->is_function) {
      // Return type
      if (obj->ty && obj->ty->return_ty) {
        fprintf(out, ",\"return_type\":");
        dump_type_json(out, obj->ty->return_ty);
      }

      // Parameters
      fputs(",\"params\":[", out);
      bool pfirst = true;
      for (Obj *p = obj->params; p; p = p->next) {
        if (!pfirst) fputc(',', out);
        pfirst = false;
        fputs("{", out);
        fprintf(out, "\"name\":");
        json_print_str(out, p->name);
        fprintf(out, ",\"type\":");
        dump_type_json(out, p->ty);
        fprintf(out, ",\"offset\":%d", p->offset);
        fputs("}", out);
      }
      fputs("]", out);

      // Body
      if (obj->body) {
        fprintf(out, ",\"body\":");
        dump_node(out, obj->body, 0);
      }

      // Locals
      fputs(",\"locals\":[", out);
      bool lfirst = true;
      for (Obj *l = obj->locals; l; l = l->next) {
        if (!lfirst) fputc(',', out);
        lfirst = false;
        fputs("{", out);
        fprintf(out, "\"name\":");
        json_print_str(out, l->name);
        fprintf(out, ",\"type\":");
        dump_type_json(out, l->ty);
        fprintf(out, ",\"offset\":%d", l->offset);
        fputs("}", out);
      }
      fputs("]", out);
    } else {
      // Global variable
      if (obj->is_tentative)
        fprintf(out, ",\"is_tentative\":true");
      if (obj->is_tls)
        fprintf(out, ",\"is_tls\":true");
      if (obj->init_data)
        fprintf(out, ",\"has_init_data\":true");
    }

    fputs("}", out);
  }

  fputs("\n]}\n", out);
}
