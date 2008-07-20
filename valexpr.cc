#include "valexpr.h"
#include "walk.h"
#include "utils.h"

namespace ledger {

value_expr amount_expr;
value_expr total_expr;

namespace expr {

std::auto_ptr<symbol_scope_t> global_scope;
datetime_t terminus;

details_t::details_t(const transaction_t& _xact)
  : entry(_xact.entry), xact(&_xact), account(xact_account(_xact))
{
  DEBUG("ledger.memory.ctors", "ctor details_t");
}

bool compute_amount(ptr_op_t expr, amount_t& amt,
		    const transaction_t * xact, ptr_op_t context)
{
  value_t result;
  try {
    expr->compute(result, xact ? details_t(*xact) : details_t(), context);
    result.cast(value_t::AMOUNT);
    amt = result.as_amount_lval();
  }
  catch (error * err) {
    if (err->context.empty() ||
	! dynamic_cast<valexpr_context *>(err->context.back()))
      err->context.push_back(new valexpr_context(expr));
    error_context * last = err->context.back();
    if (valexpr_context * ctxt = dynamic_cast<valexpr_context *>(last)) {
      ctxt->expr = expr;
      ctxt->desc = "While computing amount expression:";
    }
    throw err;
  }
  return true;
}

namespace {
  int count_leaves(ptr_op_t expr)
  {
    int count = 0;
    if (expr->kind != op_t::O_COM) {
      count = 1;
    } else {
      count += count_leaves(expr->left());
      count += count_leaves(expr->right());
    }
    return count;
  }

  ptr_op_t reduce_leaves(ptr_op_t expr, const details_t& details,
			       ptr_op_t context)
  {
    if (! expr)
      return NULL;

    value_expr temp;

    if (expr->kind != op_t::O_COM) {
      if (expr->kind < op_t::TERMINALS) {
	temp.reset(expr);
      } else {
	temp.reset(new op_t(op_t::VALUE));
	temp->set_value(value_t());
	expr->compute(temp->as_value(), details, context);
      }
    } else {
      temp.reset(new op_t(op_t::O_COM));
      temp->set_left(reduce_leaves(expr->left(), details, context));
      temp->set_right(reduce_leaves(expr->right(), details, context));
    }
    return temp.release();
  }

  ptr_op_t find_leaf(ptr_op_t context, int goal, long& found)
  {
    if (! context)
      return NULL;

    if (context->kind != op_t::O_COM) {
      if (goal == found++)
	return context;
    } else {
      ptr_op_t expr = find_leaf(context->left(), goal, found);
      if (expr)
	return expr;
      expr = find_leaf(context->right(), goal, found);
      if (expr)
	return expr;
    }
    return NULL;
  }
}

void op_t::compute(value_t& result, const details_t& details,
		   ptr_op_t context) const
{
  try {
  switch (kind) {
  case ARG_INDEX:
    throw new compute_error("Cannot directly compute an arg_index");

  case VALUE:
    result = as_value();
    break;

  case F_NOW:
    result = terminus;
    break;

  case AMOUNT:
    if (details.xact) {
      if (transaction_has_xdata(*details.xact) &&
	  transaction_xdata_(*details.xact).dflags & TRANSACTION_COMPOUND)
	result = transaction_xdata_(*details.xact).value;
      else
	result = details.xact->amount;
    }
    else if (details.account && account_has_xdata(*details.account)) {
      result = account_xdata(*details.account).value;
    }
    else {
      result = 0L;
    }
    break;

  case PRICE:
    if (details.xact) {
      bool set = false;
      if (transaction_has_xdata(*details.xact)) {
	transaction_xdata_t& xdata(transaction_xdata_(*details.xact));
	if (xdata.dflags & TRANSACTION_COMPOUND) {
	  result = xdata.value.value();
	  set = true;
	}
      }
      if (! set) {
	optional<amount_t> value = details.xact->amount.value();
	if (value)
	  result = *value;
	else
	  result = 0L;
      }
    }
    else if (details.account && account_has_xdata(*details.account)) {
      result = account_xdata(*details.account).value.value();
    }
    else {
      result = 0L;
    }
    break;

  case COST:
    if (details.xact) {
      bool set = false;
      if (transaction_has_xdata(*details.xact)) {
	transaction_xdata_t& xdata(transaction_xdata_(*details.xact));
	if (xdata.dflags & TRANSACTION_COMPOUND) {
	  result = xdata.value.cost();
	  set = true;
	}
      }

      if (! set) {
	if (details.xact->cost)
	  result = *details.xact->cost;
	else
	  result = details.xact->amount;
      }
    }
    else if (details.account && account_has_xdata(*details.account)) {
      result = account_xdata(*details.account).value.cost();
    }
    else {
      result = 0L;
    }
    break;

  case TOTAL:
    if (details.xact && transaction_has_xdata(*details.xact))
      result = transaction_xdata_(*details.xact).total;
    else if (details.account && account_has_xdata(*details.account))
      result = account_xdata(*details.account).total;
    else
      result = 0L;
    break;
  case PRICE_TOTAL:
    if (details.xact && transaction_has_xdata(*details.xact))
      result = transaction_xdata_(*details.xact).total.value();
    else if (details.account && account_has_xdata(*details.account))
      result = account_xdata(*details.account).total.value();
    else
      result = 0L;
    break;
  case COST_TOTAL:
    if (details.xact && transaction_has_xdata(*details.xact))
      result = transaction_xdata_(*details.xact).total.cost();
    else if (details.account && account_has_xdata(*details.account))
      result = account_xdata(*details.account).total.cost();
    else
      result = 0L;
    break;

  case VALUE_EXPR:
    if (amount_expr.get())
      amount_expr->compute(result, details, context);
    else
      result = 0L;
    break;
  case TOTAL_EXPR:
    if (total_expr.get())
      total_expr->compute(result, details, context);
    else
      result = 0L;
    break;

  case DATE:
    if (details.xact && transaction_has_xdata(*details.xact) &&
	is_valid(transaction_xdata_(*details.xact).date))
      result = transaction_xdata_(*details.xact).date;
    else if (details.xact)
      result = details.xact->date();
    else if (details.entry)
      result = details.entry->date();
    else
      result = terminus;
    break;

  case ACT_DATE:
    if (details.xact && transaction_has_xdata(*details.xact) &&
	is_valid(transaction_xdata_(*details.xact).date))
      result = transaction_xdata_(*details.xact).date;
    else if (details.xact)
      result = details.xact->actual_date();
    else if (details.entry)
      result = details.entry->actual_date();
    else
      result = terminus;
    break;

  case EFF_DATE:
    if (details.xact && transaction_has_xdata(*details.xact) &&
	is_valid(transaction_xdata_(*details.xact).date))
      result = transaction_xdata_(*details.xact).date;
    else if (details.xact)
      result = details.xact->effective_date();
    else if (details.entry)
      result = details.entry->effective_date();
    else
      result = terminus;
    break;

  case CLEARED:
    if (details.xact)
      result = details.xact->state == transaction_t::CLEARED;
    else
      result = false;
    break;
  case PENDING:
    if (details.xact)
      result = details.xact->state == transaction_t::PENDING;
    else
      result = false;
    break;

  case REAL:
    if (details.xact)
      result = ! (details.xact->has_flags(TRANSACTION_VIRTUAL));
    else
      result = true;
    break;

  case ACTUAL:
    if (details.xact)
      result = ! (details.xact->has_flags(TRANSACTION_AUTO));
    else
      result = true;
    break;

  case INDEX:
    if (details.xact && transaction_has_xdata(*details.xact))
      result = long(transaction_xdata_(*details.xact).index + 1);
    else if (details.account && account_has_xdata(*details.account))
      result = long(account_xdata(*details.account).count);
    else
      result = 0L;
    break;

  case COUNT:
    if (details.xact && transaction_has_xdata(*details.xact))
      result = long(transaction_xdata_(*details.xact).index + 1);
    else if (details.account && account_has_xdata(*details.account))
      result = long(account_xdata(*details.account).total_count);
    else
      result = 0L;
    break;

  case DEPTH:
    if (details.account)
      result = long(details.account->depth);
    else
      result = 0L;
    break;

  case F_PRICE: {
    long arg_index = 0;
    ptr_op_t expr = find_leaf(context, 0, arg_index);
    expr->compute(result, details, context);
    result = result.value();
    break;
  }

  case F_DATE: {
    long arg_index = 0;
    ptr_op_t expr = find_leaf(context, 0, arg_index);
    expr->compute(result, details, context);
    result = result.as_datetime_lval();
    break;
  }

  case F_DATECMP: {
    long arg_index = 0;
    ptr_op_t expr = find_leaf(context, 0, arg_index);
    expr->compute(result, details, context);
    result = result.as_datetime_lval();
    if (! result)
      break;

    arg_index = 0;
    expr = find_leaf(context, 1, arg_index);
    value_t moment;
    expr->compute(moment, details, context);
    if (moment.is_type(value_t::DATETIME)) {
      result.cast(value_t::INTEGER);
      moment.cast(value_t::INTEGER);
      result -= moment;
    } else {
      throw new compute_error("Invalid date passed to datecmp(value,date)",
			      new valexpr_context(expr));
    }
    break;
  }

  case F_YEAR:
  case F_MONTH:
  case F_DAY: {
    long arg_index = 0;
    ptr_op_t expr = find_leaf(context, 0, arg_index);
    expr->compute(result, details, context);

    if (! result.is_type(value_t::DATETIME))
      throw new compute_error("Invalid date passed to year|month|day(date)",
			      new valexpr_context(expr));

    datetime_t& moment(result.as_datetime_lval());
    switch (kind) {
    case F_YEAR:
      result = (long)moment.date().year();
      break;
    case F_MONTH:
      result = (long)moment.date().month();
      break;
    case F_DAY:
      result = (long)moment.date().day();
      break;
    default:
      break;
    }
    break;
  }

  case F_ARITH_MEAN: {
    long arg_index = 0;
    ptr_op_t expr = find_leaf(context, 0, arg_index);
    if (details.xact && transaction_has_xdata(*details.xact)) {
      expr->compute(result, details, context);
      result /= amount_t(long(transaction_xdata_(*details.xact).index + 1));
    }
    else if (details.account && account_has_xdata(*details.account) &&
	     account_xdata(*details.account).total_count) {
      expr->compute(result, details, context);
      result /= amount_t(long(account_xdata(*details.account).total_count));
    }
    else {
      result = 0L;
    }
    break;
  }

  case F_PARENT:
    if (details.account && details.account->parent)
      left()->compute(result, details_t(*details.account->parent), context);
    break;

  case F_ABS: {
    long arg_index = 0;
    ptr_op_t expr = find_leaf(context, 0, arg_index);
    expr->compute(result, details, context);
    result.abs();
    break;
  }

  case F_ROUND: {
    long arg_index = 0;
    ptr_op_t expr = find_leaf(context, 0, arg_index);
    expr->compute(result, details, context);
    result.round();
    break;
  }

  case F_COMMODITY: {
    long arg_index = 0;
    ptr_op_t expr = find_leaf(context, 0, arg_index);
    expr->compute(result, details, context);
    if (! result.is_type(value_t::AMOUNT))
      throw new compute_error("Argument to commodity() must be a commoditized amount",
			      new valexpr_context(expr));
    amount_t temp("1");
    temp.set_commodity(result.as_amount_lval().commodity());
    result = temp;
    break;
  }

  case F_SET_COMMODITY: {
    long arg_index = 0;
    ptr_op_t expr = find_leaf(context, 0, arg_index);
    value_t temp;
    expr->compute(temp, details, context);

    arg_index = 0;
    expr = find_leaf(context, 1, arg_index);
    expr->compute(result, details, context);
    if (! result.is_type(value_t::AMOUNT))
      throw new compute_error
	("Second argument to set_commodity() must be a commoditized amount",
	 new valexpr_context(expr));
    amount_t one("1");
    one.set_commodity(result.as_amount_lval().commodity());
    result = one;

    result *= temp;
    break;
  }

  case F_QUANTITY: {
    long arg_index = 0;
    ptr_op_t expr = find_leaf(context, 0, arg_index);
    expr->compute(result, details, context);

    balance_t * bal = NULL;
    switch (result.type()) {
    case value_t::BALANCE_PAIR:
      bal = &(result.as_balance_pair_lval().quantity());
      // fall through...

    case value_t::BALANCE:
      if (! bal)
	bal = &(result.as_balance_lval());

      if (bal->amounts.size() < 2) {
	result.cast(value_t::AMOUNT);
      } else {
	value_t temp;
	for (balance_t::amounts_map::const_iterator i = bal->amounts.begin();
	     i != bal->amounts.end();
	     i++) {
	  amount_t x = (*i).second;
	  x.clear_commodity();
	  temp += x;
	}
	result = temp;
	assert(temp.is_type(value_t::AMOUNT));
      }
      // fall through...

    case value_t::AMOUNT:
      result.as_amount_lval().clear_commodity();
      break;

    default:
      break;
    }
    break;
  }

#if 0
  case F_CODE_MASK:
    assert(mask);
    if (details.entry)
      result = mask->match(details.entry->code);
    else
      result = false;
    break;

  case F_PAYEE_MASK:
    assert(mask);
    if (details.entry)
      result = mask->match(details.entry->payee);
    else
      result = false;
    break;

  case F_NOTE_MASK:
    assert(mask);
    if (details.xact)
      result = mask->match(details.xact->note);
    else
      result = false;
    break;

  case F_ACCOUNT_MASK:
    assert(mask);
    if (details.account)
      result = mask->match(details.account->fullname());
    else
      result = false;
    break;

  case F_SHORT_ACCOUNT_MASK:
    assert(mask);
    if (details.account)
      result = mask->match(details.account->name);
    else
      result = false;
    break;

  case F_COMMODITY_MASK:
    assert(mask);
    if (details.xact)
      result = mask->match(details.xact->amount.commodity().base_symbol());
    else
      result = false;
    break;
#endif

  case O_ARG: {
    long arg_index = 0;
    assert(left()->kind == ARG_INDEX);
    ptr_op_t expr = find_leaf(context, left()->as_long(), arg_index);
    if (expr)
      expr->compute(result, details, context);
    else
      result = 0L;
    break;
  }

  case O_COM:
    if (! left())
      throw new compute_error("Comma operator missing left operand",
			      new valexpr_context(const_cast<op_t *>(this)));
    if (! right())
      throw new compute_error("Comma operator missing right operand",
			      new valexpr_context(const_cast<op_t *>(this)));
    left()->compute(result, details, context);
    right()->compute(result, details, context);
    break;

  case O_DEF:
    result = 0L;
    break;

  case O_REF: {
    assert(left());
    if (right()) {
      value_expr args(reduce_leaves(right(), details, context));
      left()->compute(result, details, args.get());
    } else {
      left()->compute(result, details, context);
    }
    break;
  }

  case F_VALUE: {
    long arg_index = 0;
    ptr_op_t expr = find_leaf(context, 0, arg_index);
    expr->compute(result, details, context);

    arg_index = 0;
    expr = find_leaf(context, 1, arg_index);
    value_t moment;
    expr->compute(moment, details, context);
    if (! moment.is_type(value_t::DATETIME))
      throw new compute_error("Invalid date passed to P(value,date)",
			      new valexpr_context(expr));

    result = result.value(moment.as_datetime_lval());
    break;
  }

  case O_NOT:
    left()->compute(result, details, context);
    if (result.strip_annotations())
      result = false;
    else
      result = true;
    break;

  case O_QUES: {
    assert(left());
    assert(right());
    assert(right()->kind == O_COL);
    left()->compute(result, details, context);
    if (result.strip_annotations())
      right()->left()->compute(result, details, context);
    else
      right()->right()->compute(result, details, context);
    break;
  }

  case O_AND:
    assert(left());
    assert(right());
    left()->compute(result, details, context);
    result = result.strip_annotations();
    if (result)
      right()->compute(result, details, context);
    break;

  case O_OR:
    assert(left());
    assert(right());
    left()->compute(result, details, context);
    if (! result.strip_annotations())
      right()->compute(result, details, context);
    break;

  case O_NEQ:
  case O_EQ:
  case O_LT:
  case O_LTE:
  case O_GT:
  case O_GTE: {
    assert(left());
    assert(right());
    value_t temp;
    left()->compute(temp, details, context);
    right()->compute(result, details, context);
    switch (kind) {
    case O_NEQ: result = temp != result; break;
    case O_EQ:  result = temp == result; break;
    case O_LT:  result = temp <  result; break;
    case O_LTE: result = temp <= result; break;
    case O_GT:  result = temp >  result; break;
    case O_GTE: result = temp >= result; break;
    default: assert(false); break;
    }
    break;
  }

  case O_NEG:
    assert(left());
    left()->compute(result, details, context);
    result.negate();
    break;

  case O_ADD:
  case O_SUB:
  case O_MUL:
  case O_DIV: {
    assert(left());
    assert(right());
    value_t temp;
    right()->compute(temp, details, context);
    left()->compute(result, details, context);
    switch (kind) {
    case O_ADD: result += temp; break;
    case O_SUB: result -= temp; break;
    case O_MUL: result *= temp; break;
    case O_DIV: result /= temp; break;
    default: assert(false); break;
    }
    break;
  }

  case O_PERC: {
    assert(left());
    result = "100.0%";
    value_t temp;
    left()->compute(temp, details, context);
    result *= temp;
    break;
  }

  case LAST:
  default:
    assert(false);
    break;
  }
  }
  catch (error * err) {
    if (err->context.empty() ||
	! dynamic_cast<valexpr_context *>(err->context.back()))
      err->context.push_back(new valexpr_context(const_cast<op_t *>(this)));
    throw err;
  }
}

static inline void unexpected(char c, char wanted = '\0') {
  if ((unsigned char) c == 0xff) {
    if (wanted)
      throw new value_expr_error(string("Missing '") + wanted + "'");
    else
      throw new value_expr_error("Unexpected end");
  } else {
    if (wanted)
      throw new value_expr_error(string("Invalid char '") + c +
			     "' (wanted '" + wanted + "')");
    else
      throw new value_expr_error(string("Invalid char '") + c + "'");
  }
}

ptr_op_t parse_value_term(std::istream& in, scope_t * scope,
				const short flags);

inline ptr_op_t parse_value_term(const char * p, scope_t * scope,
				       const short flags) {
  std::istringstream stream(p);
  return parse_value_term(stream, scope, flags);
}

ptr_op_t parse_value_term(std::istream& in, scope_t * scope,
				const short flags)
{
  value_expr node;

  char buf[256];
  char c = peek_next_nonws(in);

  if (flags & PARSE_VALEXPR_RELAXED) {
    if (c == '@') {
      in.get(c);
      c = peek_next_nonws(in);
    }
    else if (! (c == '(' || c == '[' || c == '{' || c == '/')) {
      amount_t temp;
      char prev_c = c;
      unsigned long pos = 0;
      // When in relaxed parsing mode, we do want to migrate commodity
      // flags, so that any precision specified by the user updates
      // the current maximum precision displayed.
      try {
	pos = (long)in.tellg();

	unsigned char parse_flags = 0;
	if (flags & PARSE_VALEXPR_NO_MIGRATE)
	  parse_flags |= AMOUNT_PARSE_NO_MIGRATE;
	if (flags & PARSE_VALEXPR_NO_REDUCE)
	  parse_flags |= AMOUNT_PARSE_NO_REDUCE;

	temp.parse(in, parse_flags);
      }
      catch (amount_error * err) {
	// If the amount had no commodity, it must be an unambiguous
	// variable reference
	if (std::strcmp(err->what(), "No quantity specified for amount") == 0) {
	  in.clear();
	  in.seekg(pos, std::ios::beg);
	  c = prev_c;
	  goto parse_ident;
	} else {
	  throw err;
	}
      }
      node.reset(new op_t(op_t::VALUE));
      node->set_value(temp);
      goto parsed;
    }
  }

 parse_ident:
  if (std::isdigit(c) || c == '.') {
    READ_INTO(in, buf, 255, c, std::isdigit(c) || c == '.');
    amount_t temp;
    temp.parse(buf, AMOUNT_PARSE_NO_MIGRATE);
    node.reset(new op_t(op_t::VALUE));
    node->set_value(temp);
    goto parsed;
  }
  else if (std::isalnum(c) || c == '_') {
    bool have_args = false;
    istream_pos_type beg;

    READ_INTO(in, buf, 255, c, std::isalnum(c) || c == '_');
    c = peek_next_nonws(in);
    if (c == '(') {
      in.get(c);
      beg = in.tellg();

      int paren_depth = 0;
      while (! in.eof()) {
	in.get(c);
	if (c == '(' || c == '{' || c == '[')
	  paren_depth++;
	else if (c == ')' || c == '}' || c == ']') {
	  if (paren_depth == 0)
	    break;
	  paren_depth--;
	}
      }
      if (c != ')')
	unexpected(c, ')');

      have_args = true;
      c = peek_next_nonws(in);
    }

    bool definition = false;
    if (c == '=') {
      in.get(c);
      if (peek_next_nonws(in) == '=') {
	in.unget();
	c = '\0';
      } else {
	definition = true;
      }
    }

    if (definition) {
      std::auto_ptr<call_scope_t> params(new call_scope_t(*scope));

      long arg_index = 0;
      if (have_args) {
	bool done = false;

	in.clear();
	in.seekg(beg, std::ios::beg);
	while (! done && ! in.eof()) {
	  char ident[32];
	  READ_INTO(in, ident, 31, c, std::isalnum(c) || c == '_');

	  c = peek_next_nonws(in);
	  in.get(c);
	  if (c != ',' && c != ')')
	    unexpected(c, ')');
	  else if (c == ')')
	    done = true;

	  // Define the parameter so that on lookup the parser will find
	  // an O_ARG value.
	  node.reset(new op_t(op_t::O_ARG));
	  node->set_left(new op_t(op_t::ARG_INDEX));
	  node->left()->set_long(arg_index++);
	  params->define(ident, node.release());
	}

	if (peek_next_nonws(in) != '=') {
	  in.get(c);
	  unexpected(c, '=');
	}
	in.get(c);
      }

      // Define the value associated with the defined identifier
      value_expr def(parse_boolean_expr(in, params.get(), flags));
      if (! def.get())
	throw new value_expr_error(string("Definition failed for '") + buf + "'");

      node.reset(new op_t(op_t::O_DEF));
      node->set_left(new op_t(op_t::ARG_INDEX));
      node->left()->set_long(arg_index);
      node->set_right(def.release());

      scope->define(buf, node.get());
    } else {
      assert(scope);
      ptr_op_t def = scope->lookup(buf);
      if (! def) {
	if (buf[1] == '\0' &&
	    (buf[0] == 'c' || buf[0] == 'C' || buf[0] == 'p' ||
	     buf[0] == 'w' || buf[0] == 'W' || buf[0] == 'e')) {
	  in.unget();
	  goto find_term;
	}
	throw new value_expr_error(string("Unknown identifier '") +
				   buf + "'");
      }
      else if (def->kind == op_t::O_DEF) {
	node.reset(new op_t(op_t::O_REF));
	node->set_left(def->right());

	unsigned int count = 0;
	if (have_args) {
	  in.clear();
	  in.seekg(beg, std::ios::beg);
	  value_expr args
	    (parse_value_expr(in, scope, flags | PARSE_VALEXPR_PARTIAL));

	  if (peek_next_nonws(in) != ')') {
	    in.get(c);
	    unexpected(c, ')');
	  }
	  in.get(c);

	  if (args.get()) {
	    count = count_leaves(args.get());
	    node->set_right(args.release());
	  }
	}

	if (count != def->left()->as_long()) {
	  std::ostringstream errmsg;
	  errmsg << "Wrong number of arguments to '" << buf
		 << "': saw " << count
		 << ", wanted " << def->left()->as_long();
	  throw new value_expr_error(errmsg.str());
	}
      }
      else {
	node.reset(def);
      }
    }
    goto parsed;
  }

 find_term:
  in.get(c);
  switch (c) {
  // Functions
  case '^':
    node.reset(new op_t(op_t::F_PARENT));
    node->set_left(parse_value_term(in, scope, flags));
    break;

  // Other
#if 0
  case 'c':
  case 'C':
  case 'p':
  case 'w':
  case 'W':
  case 'e':
  case '/': {
    bool code_mask	    = c == 'c';
    bool commodity_mask	    = c == 'C';
    bool payee_mask	    = c == 'p';
    bool note_mask	    = c == 'e';
    bool short_account_mask = c == 'w';

    if (c == '/') {
      c = peek_next_nonws(in);
      if (c == '/') {
	in.get(c);
	c = in.peek();
	if (c == '/') {
	  in.get(c);
	  c = in.peek();
	  short_account_mask = true;
	} else {
	  payee_mask = true;
	}
      }
    } else {
      in.get(c);
    }

    // Read in the regexp
    READ_INTO(in, buf, 255, c, c != '/');
    if (c != '/')
      unexpected(c, '/');

    op_t::kind_t kind;

    if (short_account_mask)
      kind = op_t::F_SHORT_ACCOUNT_MASK;
    else if (code_mask)
      kind = op_t::F_CODE_MASK;
    else if (commodity_mask)
      kind = op_t::F_COMMODITY_MASK;
    else if (payee_mask)
      kind = op_t::F_PAYEE_MASK;
    else if (note_mask)
      kind = op_t::F_NOTE_MASK;
    else
      kind = op_t::F_ACCOUNT_MASK;

    in.get(c);
    node.reset(new op_t(kind));
    node->mask = new mask_t(buf);
    break;
  }
#endif

  case '{': {
    amount_t temp;
    temp.parse(in, AMOUNT_PARSE_NO_MIGRATE);
    in.get(c);
    if (c != '}')
      unexpected(c, '}');

    node.reset(new op_t(op_t::VALUE));
    node->set_value(temp);
    break;
  }

  case '(': {
    std::auto_ptr<symbol_scope_t> locals(new symbol_scope_t(*scope));
    node.reset(parse_value_expr(in, locals.get(),
				flags | PARSE_VALEXPR_PARTIAL));
    in.get(c);
    if (c != ')')
      unexpected(c, ')');
    break;
  }

  case '[': {
    READ_INTO(in, buf, 255, c, c != ']');
    if (c != ']')
      unexpected(c, ']');
    in.get(c);

    interval_t timespan(buf);
    node.reset(new op_t(op_t::VALUE));
    node->set_value(timespan.first());
    break;
  }

  default:
    in.unget();
    break;
  }

 parsed:
  return node.release();
}

ptr_op_t parse_mul_expr(std::istream& in, scope_t * scope,
			      const short flags)
{
  value_expr node;

  if (peek_next_nonws(in) == '%') {
    char c;
    in.get(c);
    node.reset(new op_t(op_t::O_PERC));
    node->set_left(parse_value_term(in, scope, flags));
    return node.release();
  }

  node.reset(parse_value_term(in, scope, flags));

  if (node.get() && ! in.eof()) {
    char c = peek_next_nonws(in);
    while (c == '*' || c == '/') {
      in.get(c);
      switch (c) {
      case '*': {
	value_expr prev(node.release());
	node.reset(new op_t(op_t::O_MUL));
	node->set_left(prev.release());
	node->set_right(parse_value_term(in, scope, flags));
	break;
      }

      case '/': {
	value_expr prev(node.release());
	node.reset(new op_t(op_t::O_DIV));
	node->set_left(prev.release());
	node->set_right(parse_value_term(in, scope, flags));
	break;
      }
      }
      c = peek_next_nonws(in);
    }
  }

  return node.release();
}

ptr_op_t parse_add_expr(std::istream& in, scope_t * scope,
			      const short flags)
{
  value_expr node;

  if (peek_next_nonws(in) == '-') {
    char c;
    in.get(c);
    value_expr expr(parse_mul_expr(in, scope, flags));
    if (expr->kind == op_t::VALUE) {
      expr->as_value().in_place_negate();
      return expr.release();
    }
    node.reset(new op_t(op_t::O_NEG));
    node->set_left(expr.release());
    return node.release();
  }

  node.reset(parse_mul_expr(in, scope, flags));

  if (node.get() && ! in.eof()) {
    char c = peek_next_nonws(in);
    while (c == '+' || c == '-') {
      in.get(c);
      switch (c) {
      case '+': {
	value_expr prev(node.release());
	node.reset(new op_t(op_t::O_ADD));
	node->set_left(prev.release());
	node->set_right(parse_mul_expr(in, scope, flags));
	break;
      }

      case '-': {
	value_expr prev(node.release());
	node.reset(new op_t(op_t::O_SUB));
	node->set_left(prev.release());
	node->set_right(parse_mul_expr(in, scope, flags));
	break;
      }
      }
      c = peek_next_nonws(in);
    }
  }

  return node.release();
}

ptr_op_t parse_logic_expr(std::istream& in, scope_t * scope,
				const short flags)
{
  value_expr node;

  if (peek_next_nonws(in) == '!') {
    char c;
    in.get(c);
    node.reset(new op_t(op_t::O_NOT));
    node->set_left(parse_add_expr(in, scope, flags));
    return node.release();
  }

  node.reset(parse_add_expr(in, scope, flags));

  if (node.get() && ! in.eof()) {
    char c = peek_next_nonws(in);
    if (c == '!' || c == '=' || c == '<' || c == '>') {
      in.get(c);
      switch (c) {
      case '!':
      case '=': {
	bool negate = c == '!';
	if ((c = peek_next_nonws(in)) == '=')
	  in.get(c);
	else
	  unexpected(c, '=');
	value_expr prev(node.release());
	node.reset(new op_t(negate ? op_t::O_NEQ :
				    op_t::O_EQ));
	node->set_left(prev.release());
	node->set_right(parse_add_expr(in, scope, flags));
	break;
      }

      case '<': {
	value_expr prev(node.release());
	node.reset(new op_t(op_t::O_LT));
	if (peek_next_nonws(in) == '=') {
	  in.get(c);
	  node->kind = op_t::O_LTE;
	}
	node->set_left(prev.release());
	node->set_right(parse_add_expr(in, scope, flags));
	break;
      }

      case '>': {
	value_expr prev(node.release());
	node.reset(new op_t(op_t::O_GT));
	if (peek_next_nonws(in) == '=') {
	  in.get(c);
	  node->kind = op_t::O_GTE;
	}
	node->set_left(prev.release());
	node->set_right(parse_add_expr(in, scope, flags));
	break;
      }

      default:
	if (! in.eof())
	  unexpected(c);
	break;
      }
    }
  }

  return node.release();
}

ptr_op_t parse_boolean_expr(std::istream& in, scope_t * scope,
				  const short flags)
{
  value_expr node(parse_logic_expr(in, scope, flags));

  if (node.get() && ! in.eof()) {
    char c = peek_next_nonws(in);
    while (c == '&' || c == '|' || c == '?') {
      in.get(c);
      switch (c) {
      case '&': {
	value_expr prev(node.release());
	node.reset(new op_t(op_t::O_AND));
	node->set_left(prev.release());
	node->set_right(parse_logic_expr(in, scope, flags));
	break;
      }

      case '|': {
	value_expr prev(node.release());
	node.reset(new op_t(op_t::O_OR));
	node->set_left(prev.release());
	node->set_right(parse_logic_expr(in, scope, flags));
	break;
      }

      case '?': {
	value_expr prev(node.release());
	node.reset(new op_t(op_t::O_QUES));
	node->set_left(prev.release());
	node->set_right(new op_t(op_t::O_COL));
	node->right()->set_left(parse_logic_expr(in, scope, flags));
	c = peek_next_nonws(in);
	if (c != ':')
	  unexpected(c, ':');
	in.get(c);
	node->right()->set_right(parse_logic_expr(in, scope, flags));
	break;
      }

      default:
	if (! in.eof())
	  unexpected(c);
	break;
      }
      c = peek_next_nonws(in);
    }
  }

  return node.release();
}

void init_value_expr()
{
  global_scope.reset(new symbol_scope_t());
  symbol_scope_t * globals = global_scope.get();

  ptr_op_t node;

  // Basic terms
  node = new op_t(op_t::F_NOW);
  globals->define("m", node);
  globals->define("now", node);
  globals->define("today", node);

  node = new op_t(op_t::AMOUNT);
  globals->define("a", node);
  globals->define("amount", node);

  node = new op_t(op_t::PRICE);
  globals->define("i", node);
  globals->define("price", node);

  node = new op_t(op_t::COST);
  globals->define("b", node);
  globals->define("cost", node);

  node = new op_t(op_t::DATE);
  globals->define("d", node);
  globals->define("date", node);

  node = new op_t(op_t::ACT_DATE);
  globals->define("act_date", node);
  globals->define("actual_date", node);

  node = new op_t(op_t::EFF_DATE);
  globals->define("eff_date", node);
  globals->define("effective_date", node);

  node = new op_t(op_t::CLEARED);
  globals->define("X", node);
  globals->define("cleared", node);

  node = new op_t(op_t::PENDING);
  globals->define("Y", node);
  globals->define("pending", node);

  node = new op_t(op_t::REAL);
  globals->define("R", node);
  globals->define("real", node);

  node = new op_t(op_t::ACTUAL);
  globals->define("L", node);
  globals->define("actual", node);

  node = new op_t(op_t::INDEX);
  globals->define("n", node);
  globals->define("index", node);

  node = new op_t(op_t::COUNT);
  globals->define("N", node);
  globals->define("count", node);

  node = new op_t(op_t::DEPTH);
  globals->define("l", node);
  globals->define("depth", node);

  node = new op_t(op_t::TOTAL);
  globals->define("O", node);
  globals->define("total", node);

  node = new op_t(op_t::PRICE_TOTAL);
  globals->define("I", node);
  globals->define("total_price", node);

  node = new op_t(op_t::COST_TOTAL);
  globals->define("B", node);
  globals->define("total_cost", node);

  // Relating to format_t
  globals->define("t", ptr_op_t(new op_t(op_t::VALUE_EXPR)));
  globals->define("T", ptr_op_t(new op_t(op_t::TOTAL_EXPR)));

  // Functions
  node = new op_t(op_t::O_DEF);
  node->set_left(new op_t(op_t::ARG_INDEX));
  node->left()->set_long(1);
  node->set_right(new op_t(op_t::F_ABS));
  globals->define("U", node);
  globals->define("abs", node);

  node = new op_t(op_t::O_DEF);
  node->set_left(new op_t(op_t::ARG_INDEX));
  node->left()->set_long(1);
  node->set_right(new op_t(op_t::F_ROUND));
  globals->define("round", node);

  node = new op_t(op_t::O_DEF);
  node->set_left(new op_t(op_t::ARG_INDEX));
  node->left()->set_long(1);
  node->set_right(new op_t(op_t::F_QUANTITY));
  globals->define("S", node);
  globals->define("quant", node);
  globals->define("quantity", node);

  node = new op_t(op_t::O_DEF);
  node->set_left(new op_t(op_t::ARG_INDEX));
  node->left()->set_long(1);
  node->set_right(new op_t(op_t::F_COMMODITY));
  globals->define("comm", node);
  globals->define("commodity", node);

  node = new op_t(op_t::O_DEF);
  node->set_left(new op_t(op_t::ARG_INDEX));
  node->left()->set_long(2);
  node->set_right(new op_t(op_t::F_SET_COMMODITY));
  globals->define("setcomm", node);
  globals->define("set_commodity", node);

  node = new op_t(op_t::O_DEF);
  node->set_left(new op_t(op_t::ARG_INDEX));
  node->left()->set_long(1);
  node->set_right(new op_t(op_t::F_ARITH_MEAN));
  globals->define("A", node);
  globals->define("avg", node);
  globals->define("mean", node);
  globals->define("average", node);

  node = new op_t(op_t::O_DEF);
  node->set_left(new op_t(op_t::ARG_INDEX));
  node->left()->set_long(2);
  node->set_right(new op_t(op_t::F_VALUE));
  globals->define("P", node);

  parse_value_definition("@value=@P(@t,@m)", globals);
  parse_value_definition("@total_value=@P(@T,@m)", globals);
  parse_value_definition("@valueof(x)=@P(@x,@m)", globals);
  parse_value_definition("@datedvalueof(x,y)=@P(@x,@y)", globals);

  node = new op_t(op_t::O_DEF);
  node->set_left(new op_t(op_t::ARG_INDEX));
  node->left()->set_long(1);
  node->set_right(new op_t(op_t::F_PRICE));
  globals->define("priceof", node);

  node = new op_t(op_t::O_DEF);
  node->set_left(new op_t(op_t::ARG_INDEX));
  node->left()->set_long(1);
  node->set_right(new op_t(op_t::F_DATE));
  globals->define("dateof", node);

  node = new op_t(op_t::O_DEF);
  node->set_left(new op_t(op_t::ARG_INDEX));
  node->left()->set_long(2);
  node->set_right(new op_t(op_t::F_DATECMP));
  globals->define("datecmp", node);

  node = new op_t(op_t::O_DEF);
  node->set_left(new op_t(op_t::ARG_INDEX));
  node->left()->set_long(1);
  node->set_right(new op_t(op_t::F_YEAR));
  globals->define("yearof", node);

  node = new op_t(op_t::O_DEF);
  node->set_left(new op_t(op_t::ARG_INDEX));
  node->left()->set_long(1);
  node->set_right(new op_t(op_t::F_MONTH));
  globals->define("monthof", node);

  node = new op_t(op_t::O_DEF);
  node->set_left(new op_t(op_t::ARG_INDEX));
  node->left()->set_long(1);
  node->set_right(new op_t(op_t::F_DAY));
  globals->define("dayof", node);

  parse_value_definition("@year=@yearof(@d)", globals);
  parse_value_definition("@month=@monthof(@d)", globals);
  parse_value_definition("@day=@dayof(@d)", globals);

  // Macros
  node = parse_value_expr("@P(@a,@d)");
  globals->define("v", node);
  globals->define("market", node);

  node = parse_value_expr("@P(@O,@d)");
  globals->define("V", node);
  globals->define("total_market", node);

  node = parse_value_expr("@v-@b");
  globals->define("g", node);
  globals->define("gain", node);

  node = parse_value_expr("@V-@B");
  globals->define("G", node);
  globals->define("total_gain", node);

  parse_value_definition("@min(x,y)=@x<@y?@x:@y", globals);
  parse_value_definition("@max(x,y)=@x>@y?@x:@y", globals);
}

ptr_op_t parse_value_expr(std::istream& in, scope_t * scope,
				const short flags)
{
  if (! global_scope.get())
    init_value_expr();

  std::auto_ptr<symbol_scope_t>
    this_scope(new symbol_scope_t(scope ? *scope : *global_scope.get()));
  value_expr node;
  node.reset(parse_boolean_expr(in, this_scope.get(), flags));

  if (node.get() && ! in.eof()) {
    char c = peek_next_nonws(in);
    while (c == ',') {
      in.get(c);
      switch (c) {
      case ',': {
	value_expr prev(node.release());
	node.reset(new op_t(op_t::O_COM));
	node->set_left(prev.release());
	node->set_right(parse_logic_expr(in, this_scope.get(), flags));
	break;
      }

      default:
	if (! in.eof())
	  unexpected(c);
	break;
      }
      c = peek_next_nonws(in);
    }
  }

  char c;
  if (! node.get()) {
    in.get(c);
    if (in.eof())
      throw new value_expr_error(string("Failed to parse value expression"));
    else
      unexpected(c);
  } else if (! (flags & PARSE_VALEXPR_PARTIAL)) {
    in.get(c);
    if (! in.eof())
      unexpected(c);
    else
      in.unget();
  }

  return node.release();
}

void valexpr_context::describe(std::ostream& out) const throw()
{
  if (! expr) {
    out << "valexpr_context expr not set!" << std::endl;
    return;
  }

  if (! desc.empty())
    out << desc << std::endl;

  out << "  ";
  unsigned long start = (long)out.tellp() - 1;
  unsigned long begin;
  unsigned long end;
  bool found = print_value_expr(out, expr, true, error_node, &begin, &end);
  out << std::endl;
  if (found) {
    out << "  ";
    for (unsigned int i = 0; i < end - start; i++) {
      if (i >= begin - start)
	out << "^";
      else
	out << " ";
    }
    out << std::endl;
  }
}

bool print_value_expr(std::ostream&   out,
		      const ptr_op_t  node,
		      const bool      relaxed,
		      const ptr_op_t  op_to_find,
		      unsigned long * start_pos,
		      unsigned long * end_pos)
{
  bool found = false;

  if (start_pos && node == op_to_find) {
    *start_pos = (long)out.tellp() - 1;
    found = true;
  }

  string symbol;

  switch (node->kind) {
  case op_t::ARG_INDEX:
    out << node->as_long();
    break;

  case op_t::VALUE:
    switch (node->as_value().type()) {
    case value_t::BOOLEAN:
      assert(false);
      break;
    case value_t::DATETIME:
      out << '[' << node->as_value().as_datetime() << ']';
      break;
    case value_t::INTEGER:
    case value_t::AMOUNT:
      if (! relaxed)
	out << '{';
      out << node->as_value();
      if (! relaxed)
	out << '}';
      break;
    //case value_t::BALANCE:
    //case value_t::BALANCE_PAIR:
    default:
      assert(false);
      break;
    }
    break;

  case op_t::AMOUNT:
    symbol = "amount"; break;
  case op_t::PRICE:
    symbol = "price"; break;
  case op_t::COST:
    symbol = "cost"; break;
  case op_t::DATE:
    symbol = "date"; break;
  case op_t::ACT_DATE:
    symbol = "actual_date"; break;
  case op_t::EFF_DATE:
    symbol = "effective_date"; break;
  case op_t::CLEARED:
    symbol = "cleared"; break;
  case op_t::PENDING:
    symbol = "pending"; break;
  case op_t::REAL:
    symbol = "real"; break;
  case op_t::ACTUAL:
    symbol = "actual"; break;
  case op_t::INDEX:
    symbol = "index"; break;
  case op_t::COUNT:
    symbol = "count"; break;
  case op_t::DEPTH:
    symbol = "depth"; break;
  case op_t::TOTAL:
    symbol = "total"; break;
  case op_t::PRICE_TOTAL:
    symbol = "total_price"; break;
  case op_t::COST_TOTAL:
    symbol = "total_cost"; break;
  case op_t::F_NOW:
    symbol = "now"; break;

  case op_t::VALUE_EXPR:
    if (print_value_expr(out, amount_expr.get(), relaxed,
			 op_to_find, start_pos, end_pos))
      found = true;
    break;
  case op_t::TOTAL_EXPR:
    if (print_value_expr(out, total_expr.get(), relaxed,
			 op_to_find, start_pos, end_pos))
      found = true;
    break;

  case op_t::F_ARITH_MEAN:
    symbol = "average"; break;
  case op_t::F_ABS:
    symbol = "abs"; break;
  case op_t::F_QUANTITY:
    symbol = "quantity"; break;
  case op_t::F_COMMODITY:
    symbol = "commodity"; break;
  case op_t::F_SET_COMMODITY:
    symbol = "set_commodity"; break;
  case op_t::F_VALUE:
    symbol = "valueof"; break;
  case op_t::F_PRICE:
    symbol = "priceof"; break;
  case op_t::F_DATE:
    symbol = "dateof"; break;
  case op_t::F_DATECMP:
    symbol = "datecmp"; break;
  case op_t::F_YEAR:
    symbol = "yearof"; break;
  case op_t::F_MONTH:
    symbol = "monthof"; break;
  case op_t::F_DAY:
    symbol = "dayof"; break;

#if 0
  case op_t::F_CODE_MASK:
    out << "c/" << node->mask->expr.str() << "/";
    break;
  case op_t::F_PAYEE_MASK:
    out << "p/" << node->mask->expr.str() << "/";
    break;
  case op_t::F_NOTE_MASK:
    out << "e/" << node->mask->expr.str() << "/";
    break;
  case op_t::F_ACCOUNT_MASK:
    out << "W/" << node->mask->expr.str() << "/";
    break;
  case op_t::F_SHORT_ACCOUNT_MASK:
    out << "w/" << node->mask->expr.str() << "/";
    break;
  case op_t::F_COMMODITY_MASK:
    out << "C/" << node->mask->expr.str() << "/";
    break;
#endif

  case op_t::O_NOT:
    out << "!";
    if (print_value_expr(out, node->left(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    break;
  case op_t::O_NEG:
    out << "-";
    if (print_value_expr(out, node->left(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    break;
  case op_t::O_PERC:
    out << "%";
    if (print_value_expr(out, node->left(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    break;

  case op_t::O_ARG:
    out << "@arg" << node->as_long();
    break;
  case op_t::O_DEF:
    out << "<def args=\"";
    if (print_value_expr(out, node->left(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << "\" value=\"";
    if (print_value_expr(out, node->right(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << "\">";
    break;

  case op_t::O_REF:
    if (print_value_expr(out, node->left(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    if (node->right()) {
      out << "(";
      if (print_value_expr(out, node->right(), relaxed, op_to_find, start_pos, end_pos))
	found = true;
      out << ")";
    }
    break;

  case op_t::O_COM:
    if (node->left() &&
	print_value_expr(out, node->left(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << ", ";
    if (node->right() &&
	print_value_expr(out, node->right(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    break;
  case op_t::O_QUES:
    out << "(";
    if (print_value_expr(out, node->left(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << " ? ";
    if (print_value_expr(out, node->right(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << ")";
    break;
  case op_t::O_COL:
    if (print_value_expr(out, node->left(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << " : ";
    if (print_value_expr(out, node->right(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    break;

  case op_t::O_AND:
    out << "(";
    if (print_value_expr(out, node->left(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << " & ";
    if (print_value_expr(out, node->right(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << ")";
    break;
  case op_t::O_OR:
    out << "(";
    if (print_value_expr(out, node->left(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << " | ";
    if (print_value_expr(out, node->right(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << ")";
    break;

  case op_t::O_NEQ:
    out << "(";
    if (print_value_expr(out, node->left(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << " != ";
    if (print_value_expr(out, node->right(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << ")";
    break;
  case op_t::O_EQ:
    out << "(";
    if (print_value_expr(out, node->left(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << " == ";
    if (print_value_expr(out, node->right(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << ")";
    break;
  case op_t::O_LT:
    out << "(";
    if (print_value_expr(out, node->left(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << " < ";
    if (print_value_expr(out, node->right(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << ")";
    break;
  case op_t::O_LTE:
    out << "(";
    if (print_value_expr(out, node->left(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << " <= ";
    if (print_value_expr(out, node->right(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << ")";
    break;
  case op_t::O_GT:
    out << "(";
    if (print_value_expr(out, node->left(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << " > ";
    if (print_value_expr(out, node->right(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << ")";
    break;
  case op_t::O_GTE:
    out << "(";
    if (print_value_expr(out, node->left(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << " >= ";
    if (print_value_expr(out, node->right(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << ")";
    break;

  case op_t::O_ADD:
    out << "(";
    if (print_value_expr(out, node->left(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << " + ";
    if (print_value_expr(out, node->right(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << ")";
    break;
  case op_t::O_SUB:
    out << "(";
    if (print_value_expr(out, node->left(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << " - ";
    if (print_value_expr(out, node->right(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << ")";
    break;
  case op_t::O_MUL:
    out << "(";
    if (print_value_expr(out, node->left(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << " * ";
    if (print_value_expr(out, node->right(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << ")";
    break;
  case op_t::O_DIV:
    out << "(";
    if (print_value_expr(out, node->left(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << " / ";
    if (print_value_expr(out, node->right(), relaxed, op_to_find, start_pos, end_pos))
      found = true;
    out << ")";
    break;

  case op_t::LAST:
  default:
    assert(false);
    break;
  }

  if (! symbol.empty()) {
    if (amount_t::current_pool->find(symbol))
      out << '@';
    out << symbol;
  }

  if (end_pos && node == op_to_find)
    *end_pos = (long)out.tellp() - 1;

  return found;
}

void dump_value_expr(std::ostream& out, const ptr_op_t node,
		     const int depth)
{
  out.setf(std::ios::left);
  out.width(10);
  out << node << " ";

  for (int i = 0; i < depth; i++)
    out << " ";

  switch (node->kind) {
  case op_t::ARG_INDEX:
    out << "ARG_INDEX - " << node->as_long();
    break;
  case op_t::VALUE:
    out << "VALUE - " << node->as_value();
    break;

  case op_t::AMOUNT: out << "AMOUNT"; break;
  case op_t::PRICE: out << "PRICE"; break;
  case op_t::COST: out << "COST"; break;
  case op_t::DATE: out << "DATE"; break;
  case op_t::ACT_DATE: out << "ACT_DATE"; break;
  case op_t::EFF_DATE: out << "EFF_DATE"; break;
  case op_t::CLEARED: out << "CLEARED"; break;
  case op_t::PENDING: out << "PENDING"; break;
  case op_t::REAL: out << "REAL"; break;
  case op_t::ACTUAL: out << "ACTUAL"; break;
  case op_t::INDEX: out << "INDEX"; break;
  case op_t::COUNT: out << "COUNT"; break;
  case op_t::DEPTH: out << "DEPTH"; break;
  case op_t::TOTAL: out << "TOTAL"; break;
  case op_t::PRICE_TOTAL: out << "PRICE_TOTAL"; break;
  case op_t::COST_TOTAL: out << "COST_TOTAL"; break;

  case op_t::VALUE_EXPR: out << "VALUE_EXPR"; break;
  case op_t::TOTAL_EXPR: out << "TOTAL_EXPR"; break;

  case op_t::F_NOW: out << "F_NOW"; break;
  case op_t::F_ARITH_MEAN: out << "F_ARITH_MEAN"; break;
  case op_t::F_ABS: out << "F_ABS"; break;
  case op_t::F_QUANTITY: out << "F_QUANTITY"; break;
  case op_t::F_COMMODITY: out << "F_COMMODITY"; break;
  case op_t::F_SET_COMMODITY: out << "F_SET_COMMODITY"; break;
  case op_t::F_CODE_MASK: out << "F_CODE_MASK"; break;
  case op_t::F_PAYEE_MASK: out << "F_PAYEE_MASK"; break;
  case op_t::F_NOTE_MASK: out << "F_NOTE_MASK"; break;
  case op_t::F_ACCOUNT_MASK:
    out << "F_ACCOUNT_MASK"; break;
  case op_t::F_SHORT_ACCOUNT_MASK:
    out << "F_SHORT_ACCOUNT_MASK"; break;
  case op_t::F_COMMODITY_MASK:
    out << "F_COMMODITY_MASK"; break;
  case op_t::F_VALUE: out << "F_VALUE"; break;
  case op_t::F_PRICE: out << "F_PRICE"; break;
  case op_t::F_DATE: out << "F_DATE"; break;
  case op_t::F_DATECMP: out << "F_DATECMP"; break;
  case op_t::F_YEAR: out << "F_YEAR"; break;
  case op_t::F_MONTH: out << "F_MONTH"; break;
  case op_t::F_DAY: out << "F_DAY"; break;

  case op_t::O_NOT: out << "O_NOT"; break;
  case op_t::O_ARG: out << "O_ARG"; break;
  case op_t::O_DEF: out << "O_DEF"; break;
  case op_t::O_REF: out << "O_REF"; break;
  case op_t::O_COM: out << "O_COM"; break;
  case op_t::O_QUES: out << "O_QUES"; break;
  case op_t::O_COL: out << "O_COL"; break;
  case op_t::O_AND: out << "O_AND"; break;
  case op_t::O_OR: out << "O_OR"; break;
  case op_t::O_NEQ: out << "O_NEQ"; break;
  case op_t::O_EQ: out << "O_EQ"; break;
  case op_t::O_LT: out << "O_LT"; break;
  case op_t::O_LTE: out << "O_LTE"; break;
  case op_t::O_GT: out << "O_GT"; break;
  case op_t::O_GTE: out << "O_GTE"; break;
  case op_t::O_NEG: out << "O_NEG"; break;
  case op_t::O_ADD: out << "O_ADD"; break;
  case op_t::O_SUB: out << "O_SUB"; break;
  case op_t::O_MUL: out << "O_MUL"; break;
  case op_t::O_DIV: out << "O_DIV"; break;
  case op_t::O_PERC: out << "O_PERC"; break;

  case op_t::LAST:
  default:
    assert(false);
    break;
  }

  out << " (" << node->refc << ')' << std::endl;

  if (node->kind > op_t::TERMINALS) {
    dump_value_expr(out, node->left(), depth + 1);
    if (node->right())
      dump_value_expr(out, node->right(), depth + 1);
  }
}

} // namespace expr
} // namespace ledger
