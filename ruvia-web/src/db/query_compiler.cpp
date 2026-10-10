#include <algorithm>
#include <array>
#include <stdexcept>
#include <unordered_map>

#include "ruvia/core/memory/pmr_resource.h"

#include "query_storage.h"

namespace ruvia::detail {
namespace {

bool is_mariadb_bare_function_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    const auto is_ascii_letter = [](char value) noexcept {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
    };
    if (!is_ascii_letter(name.front()) && name.front() != '_') {
        return false;
    }
    return std::ranges::all_of(name.substr(1), [&](char value) noexcept {
        return is_ascii_letter(value) || (value >= '0' && value <= '9') || value == '_' || value == '$';
    });
}

class query_sql_writer final {
public:
    query_sql_writer(db_driver driver, std::pmr::memory_resource* resource, db_parameter_mode mode)
        : sql_(resource),
          params_(resource),
          driver_(driver),
          mode_(mode),
          parameter_positions_(resource) {
        require_db_dialect(driver);
    }

    std::pmr::string sql_;
    std::pmr::vector<db_value> params_;
    [[nodiscard]] db_driver driver() const noexcept {
        return driver_;
    }

    bool pg() const noexcept {
        return driver_ == db_driver::postgresql;
    }

    [[noreturn]] static void unsupported(std::string_view feature) {
        throw std::invalid_argument(std::string(feature) + " is not supported by this database dialect");
    }

    void require_pg(std::string_view feature) const {
        if (!pg()) {
            unsupported(feature);
        }
    }

    void identifier(std::string_view name) {
        append_db_identifier(sql_, name, driver_);
    }

    void qualified(std::string_view name) {
        append_db_qualified_identifier(sql_, name, driver_);
    }

    void function_name(std::string_view name) {
        if (driver_ == db_driver::mariadb && is_mariadb_bare_function_name(name)) {
            sql_ += name;
        } else {
            qualified(name);
        }
    }

    void bound(const db_value& value, const db_query_node* node = nullptr) {
        if (mode_ == db_parameter_mode::literal || db_value_access::type(value) == db_value_type::null) {
            append_db_literal(sql_, value, driver_);
            return;
        }
        if (pg() && node != nullptr) {
            if (const auto found = parameter_positions_.find(node); found != parameter_positions_.end()) {
                sql_ += '$';
                append_db_number(sql_, static_cast<std::uint64_t>(found->second));
                return;
            }
        }
        params_.push_back(clone_db_value_for_resource(value, params_.get_allocator().resource()));
        if (pg() && node != nullptr) {
            parameter_positions_.emplace(node, params_.size());
        }
        if (pg()) {
            sql_ += '$';
            append_db_number(sql_, static_cast<std::uint64_t>(params_.size()));
        } else {
            sql_ += '?';
        }
    }

    void number(std::uint64_t value) {
        bound(db_value(static_cast<std::int64_t>(value)));
    }

    void type(const db_stored_type& value) {
        append_db_type_name(sql_, value.data_type_, value.custom_name_, value.length_, value.precision_,
            value.scale_, value.array_, driver_, db_sql_type_usage::cast);
    }

private:
    db_driver driver_;
    db_parameter_mode mode_;
    std::pmr::unordered_map<const db_query_node*, std::size_t> parameter_positions_;
};

}  // namespace

class db_query_compiler final {
public:
    db_query_compiler(db_driver driver, std::pmr::memory_resource* resource, db_parameter_mode mode)
        : output_(driver, resource, mode) {}

    query_sql_writer output_;

    void expression(db_expression value) {
        if (value.empty()) {
            throw std::invalid_argument("empty database expression");
        }
        expr(*value.owner_, value.node_, 0);
    }
    void query(const db_query_storage& s, std::size_t depth = 0) {
        require_depth(depth);
        validate_query_shape(s);
        if (!s.ctes_.empty()) {
            if (!output_.pg() && s.kind_ != db_query_kind::select) {
                output_.unsupported("data-changing WITH");
            }
            output_.sql_ += "WITH ";
            if (std::ranges::any_of(s.ctes_, [](const auto& cte) { return cte.recursive_; })) {
                output_.sql_ += "RECURSIVE ";
            }
            separated(s.ctes_, [&](const auto& cte) {
                output_.identifier(cte.name_);
                if (!cte.columns_.empty()) {
                    output_.sql_ += " (";
                    identifiers(cte.columns_);
                    output_.sql_ += ')';
                }
                output_.sql_ += " AS ";
                switch (cte.materialization_) {
                    case db_materialization::default_value:
                        break;
                    case db_materialization::materialized:
                        output_.require_pg("materialized CTE");
                        output_.sql_ += "MATERIALIZED ";
                        break;
                    case db_materialization::not_materialized:
                        output_.require_pg("not-materialized CTE");
                        output_.sql_ += "NOT MATERIALIZED ";
                        break;
                }
                const auto& child_value = nested(s, cte.query_);
                if (child_value.kind_ != db_query_kind::select && child_value.kind_ != db_query_kind::values) {
                    output_.require_pg("data-changing CTE");
                    if (depth != 0) {
                        throw std::invalid_argument("data-changing CTE requires a top-level WITH statement");
                    }
                }
                output_.sql_ += '(';
                query(child_value, depth + 1);
                output_.sql_ += ')';
            });
            output_.sql_ += ' ';
        }
        for (std::size_t i = 0; i < s.set_operations_.size(); ++i) {
            output_.sql_ += '(';
        }
        body(s, depth + 1);
        for (const auto& operation : s.set_operations_) {
            output_.sql_ += ')';
            switch (operation.operation_) {
                case db_set_operation::union_value:
                    output_.sql_ += " UNION ";
                    break;
                case db_set_operation::union_all:
                    output_.sql_ += " UNION ALL ";
                    break;
                case db_set_operation::intersect:
                    output_.sql_ += " INTERSECT ";
                    break;
                case db_set_operation::intersect_all:
                    output_.require_pg("INTERSECT ALL");
                    output_.sql_ += " INTERSECT ALL ";
                    break;
                case db_set_operation::except:
                    output_.sql_ += " EXCEPT ";
                    break;
                case db_set_operation::except_all:
                    output_.require_pg("EXCEPT ALL");
                    output_.sql_ += " EXCEPT ALL ";
                    break;
            }
            output_.sql_ += '(';
            query(nested(s, operation.query_), depth + 1);
            output_.sql_ += ')';
        }
        if (!s.orders_.empty()) {
            output_.sql_ += " ORDER BY ";
            orders(s, s.orders_, depth + 1);
        }
        if (s.limit_) {
            output_.sql_ += " LIMIT ";
            output_.number(*s.limit_);
        } else if (s.offset_ && !output_.pg()) {
            output_.sql_ += " LIMIT 18446744073709551615";
        }
        if (s.offset_) {
            output_.sql_ += " OFFSET ";
            output_.number(*s.offset_);
        }
        if (s.lock_) {
            const auto& lock = *s.lock_;
            switch (lock.mode_) {
                case db_row_lock::update:
                    output_.sql_ += " FOR UPDATE";
                    break;
                case db_row_lock::no_key_update:
                    output_.require_pg("FOR NO KEY UPDATE");
                    output_.sql_ += " FOR NO KEY UPDATE";
                    break;
                case db_row_lock::share:
                    output_.sql_ += output_.pg() ? " FOR SHARE" : " LOCK IN SHARE MODE";
                    break;
                case db_row_lock::key_share:
                    output_.require_pg("FOR KEY SHARE");
                    output_.sql_ += " FOR KEY SHARE";
                    break;
            }
            if (!lock.tables_.empty()) {
                output_.require_pg("row lock OF");
                output_.sql_ += " OF ";
                identifiers(lock.tables_);
            }
            if (lock.nowait_) {
                output_.sql_ += " NOWAIT";
            }
            if (lock.skip_locked_) {
                if (!output_.pg() && lock.mode_ != db_row_lock::update) {
                    output_.unsupported("SKIP LOCKED for a shared lock");
                }
                output_.sql_ += " SKIP LOCKED";
            }
        }
        if (!s.returning_.empty()) {
            output_.require_pg("RETURNING");
            output_.sql_ += " RETURNING ";
            expressions(s, s.returning_, depth + 1, true);
        }
    }

private:
    static void require_depth(std::size_t depth) {
        if (depth > 256) {
            throw std::length_error("database query nesting exceeds 256 levels");
        }
    }
    static const db_query_storage& nested(const db_query_storage& s, std::size_t index) {
        return s.queries_.at(index).storage();
    }
    template <class range_type, class fn_type>
    void separated(const range_type& range, fn_type&& fn) {
        bool first = true;
        for (const auto& item : range) {
            if (!first) {
                output_.sql_ += ", ";
            }
            first = false;
            fn(item);
        }
    }
    template <class range_type>
    void identifiers(const range_type& range) {
        separated(range, [&](const auto& name) { output_.identifier(name); });
    }
    template <class range_type>
    void expressions(const db_query_storage& s, const range_type& range, std::size_t depth, bool aliases = false) {
        separated(range, [&](std::size_t index) { expr(s, index, depth, aliases); });
    }
    void orders(const db_query_storage& s, const std::pmr::vector<db_stored_order>& terms, std::size_t depth) {
        separated(terms, [&](const auto& term) {
            if (!output_.pg() && term.nulls_ != db_nulls_order::default_value) {
                output_.sql_ += '(';
                expr(s, term.expression_, depth);
                output_.sql_ += " IS NULL)";
                output_.sql_ += term.nulls_ == db_nulls_order::first ? " DESC, " : " ASC, ";
            }
            expr(s, term.expression_, depth);
            output_.sql_ += term.direction_ == db_order_direction::asc ? " ASC" : " DESC";
            if (output_.pg()) {
                if (term.nulls_ == db_nulls_order::first) {
                    output_.sql_ += " NULLS FIRST";
                }
                if (term.nulls_ == db_nulls_order::last) {
                    output_.sql_ += " NULLS LAST";
                }
            }
        });
    }
    void boundary(const db_window_boundary& boundary) {
        if (boundary.kind_ != db_frame_boundary::preceding && boundary.kind_ != db_frame_boundary::following && boundary.offset_ != 0) {
            throw std::invalid_argument("a non-offset window frame boundary has an offset");
        }
        switch (boundary.kind_) {
            case db_frame_boundary::unbounded_preceding:
                output_.sql_ += "UNBOUNDED PRECEDING";
                break;
            case db_frame_boundary::preceding:
                append_db_number(output_.sql_, boundary.offset_);
                output_.sql_ += " PRECEDING";
                break;
            case db_frame_boundary::current_row:
                output_.sql_ += "CURRENT ROW";
                break;
            case db_frame_boundary::following:
                append_db_number(output_.sql_, boundary.offset_);
                output_.sql_ += " FOLLOWING";
                break;
            case db_frame_boundary::unbounded_following:
                output_.sql_ += "UNBOUNDED FOLLOWING";
                break;
        }
    }
    // Imports own their nodes. Equal grouped expressions must nevertheless use
    // the same PostgreSQL parameter nodes in SELECT, GROUP BY and HAVING.
    static bool same_expression(const db_query_storage& s, std::size_t lhs, std::size_t rhs, std::size_t depth = 0) {
        if (lhs == rhs) {
            return true;
        }
        if (lhs == no_db_node || rhs == no_db_node || depth > 256) {
            return false;
        }
        const auto& a = s.nodes_.at(lhs);
        const auto& b = s.nodes_.at(rhs);
        if (a.kind_ != b.kind_ || a.text_ != b.text_ || a.qualifier_ != b.qualifier_ || a.binary_ != b.binary_ ||
            a.unary_ != b.unary_ || a.date_part_ != b.date_part_ || a.flag_ != b.flag_ ||
            a.query_ != no_db_node || b.query_ != no_db_node || a.args_.size() != b.args_.size() ||
            a.named_.size() != b.named_.size() || a.orders_.size() != b.orders_.size() ||
            a.type_.data_type_ != b.type_.data_type_ || a.type_.custom_name_ != b.type_.custom_name_ ||
            a.type_.length_ != b.type_.length_ || a.type_.precision_ != b.type_.precision_ || a.type_.scale_ != b.type_.scale_ ||
            a.type_.array_ != b.type_.array_ || a.frame_.has_value() != b.frame_.has_value()) {
            return false;
        }
        if (a.frame_ && (a.frame_->kind_ != b.frame_->kind_ || a.frame_->start_.kind_ != b.frame_->start_.kind_ ||
                            a.frame_->start_.offset_ != b.frame_->start_.offset_ || a.frame_->end_.kind_ != b.frame_->end_.kind_ ||
                            a.frame_->end_.offset_ != b.frame_->end_.offset_)) {
            return false;
        }
        if (a.kind_ == db_node_kind::value) {
            if (db_value_access::type(a.value_) != db_value_access::type(b.value_)) {
                return false;
            }
            switch (db_value_access::type(a.value_)) {
                case db_value_type::null:
                    break;
                case db_value_type::string:
                    if (db_value_access::text(a.value_) != db_value_access::text(b.value_)) {
                        return false;
                    }
                    break;
                case db_value_type::signed_value:
                    if (db_value_access::signed_value(a.value_) != db_value_access::signed_value(b.value_)) {
                        return false;
                    }
                    break;
                case db_value_type::unsigned_value:
                    if (db_value_access::unsigned_value(a.value_) != db_value_access::unsigned_value(b.value_)) {
                        return false;
                    }
                    break;
                case db_value_type::double_value:
                    if (db_value_access::get_double_value(a.value_) != db_value_access::get_double_value(b.value_)) {
                        return false;
                    }
                    break;
                case db_value_type::bool_value:
                    if (db_value_access::get_bool_value(a.value_) != db_value_access::get_bool_value(b.value_)) {
                        return false;
                    }
                    break;
            }
        }
        const auto same = [&](std::size_t x, std::size_t y) { return same_expression(s, x, y, depth + 1); };
        if (!same(a.left_, b.left_) || !same(a.right_, b.right_)) {
            return false;
        }
        for (std::size_t i = 0; i < a.args_.size(); ++i) {
            if (!same(a.args_[i], b.args_[i])) {
                return false;
            }
        }
        for (std::size_t i = 0; i < a.named_.size(); ++i) {
            if (a.named_[i].name_ != b.named_[i].name_ || !same(a.named_[i].expression_, b.named_[i].expression_)) {
                return false;
            }
        }
        for (std::size_t i = 0; i < a.orders_.size(); ++i) {
            if (a.orders_[i].direction_ != b.orders_[i].direction_ || a.orders_[i].nulls_ != b.orders_[i].nulls_ ||
                !same(a.orders_[i].expression_, b.orders_[i].expression_)) {
                return false;
            }
        }
        return true;
    }
    static std::size_t group_expression(const db_query_storage& s, std::size_t index) {
        for (const auto group : s.groups_) {
            if (!same_expression(s, group, index)) {
                continue;
            }
            for (auto projection : s.projections_) {
                if (s.nodes_[projection].kind_ == db_node_kind::alias) {
                    projection = s.nodes_[projection].left_;
                }
                if (same_expression(s, group, projection)) {
                    return projection;
                }
            }
            return group;
        }
        return index;
    }
    void expr(const db_query_storage& s, std::size_t index, std::size_t depth, bool aliases = false) {
        require_depth(depth);
        if (output_.pg() && !s.groups_.empty()) {
            index = group_expression(s, index);
        }
        const auto& n = s.nodes_.at(index);
        const auto child_value = [&](std::size_t id) { expr(s, id, depth + 1); };
        switch (n.kind_) {
            case db_node_kind::sql:
                output_.sql_ += '(';
                for (const auto& part : n.named_) {
                    output_.sql_ += part.name_;
                    child_value(part.expression_);
                }
                output_.sql_ += n.text_;
                output_.sql_ += ')';
                break;
            case db_node_kind::column:
            case db_node_kind::star:
                if (!n.qualifier_.empty()) {
                    output_.qualified(n.qualifier_);
                    output_.sql_ += '.';
                }
                if (n.kind_ == db_node_kind::star) {
                    output_.sql_ += '*';
                } else {
                    output_.identifier(n.text_);
                }
                break;
            case db_node_kind::value:
                output_.bound(n.value_, &n);
                break;
            case db_node_kind::default_value:
                output_.sql_ += "DEFAULT";
                break;
            case db_node_kind::excluded:
                if (output_.pg()) {
                    output_.sql_ += "excluded.";
                    output_.identifier(n.text_);
                } else {
                    output_.sql_ += "VALUES(";
                    output_.identifier(n.text_);
                    output_.sql_ += ')';
                }
                break;
            case db_node_kind::function:
            case db_node_kind::aggregate:
            case db_node_kind::coalesce:
            case db_node_kind::null_if:
            case db_node_kind::greatest:
            case db_node_kind::least: {
                switch (n.kind_) {
                    case db_node_kind::coalesce:
                        output_.sql_ += "COALESCE";
                        break;
                    case db_node_kind::null_if:
                        output_.sql_ += "NULLIF";
                        break;
                    case db_node_kind::greatest:
                        output_.sql_ += "GREATEST";
                        break;
                    case db_node_kind::least:
                        output_.sql_ += "LEAST";
                        break;
                    default:
                        output_.function_name(n.text_);
                        break;
                }
                output_.sql_ += '(';
                if (n.kind_ == db_node_kind::aggregate && n.flag_) {
                    output_.sql_ += "DISTINCT ";
                }
                expressions(s, n.args_, depth + 1);
                if (!n.named_.empty()) {
                    output_.require_pg("named function arguments");
                    if (!n.args_.empty()) {
                        output_.sql_ += ", ";
                    }
                    separated(n.named_, [&](const auto& arg) { output_.identifier(arg.name_); output_.sql_ += " => "; child_value(arg.expression_); });
                }
                if (!n.orders_.empty()) {
                    output_.require_pg("ordered aggregate");
                    output_.sql_ += " ORDER BY ";
                    orders(s, n.orders_, depth + 1);
                }
                output_.sql_ += ')';
                break;
            }
            case db_node_kind::binary:
                binary(s, n, depth + 1);
                break;
            case db_node_kind::unary: {
                output_.sql_ += '(';
                switch (n.unary_) {
                    case db_unary_operator::not_value:
                        output_.sql_ += "NOT ";
                        child_value(n.left_);
                        break;
                    case db_unary_operator::negate:
                        // Keep the operator apart from its operand: a negative
                        // literal would otherwise render "--5", which
                        // PostgreSQL lexes as a line comment, and "~-5", which
                        // it lexes as the single operator "~-".
                        output_.sql_ += "- ";
                        child_value(n.left_);
                        break;
                    case db_unary_operator::bit_not:
                        output_.sql_ += "~ ";
                        child_value(n.left_);
                        break;
                    default:
                        child_value(n.left_);
                        switch (n.unary_) {
                            case db_unary_operator::is_null:
                                output_.sql_ += " IS NULL";
                                break;
                            case db_unary_operator::is_not_null:
                                output_.sql_ += " IS NOT NULL";
                                break;
                            case db_unary_operator::is_true:
                                output_.sql_ += " IS TRUE";
                                break;
                            case db_unary_operator::is_false:
                                output_.sql_ += " IS FALSE";
                                break;
                            case db_unary_operator::is_not_true:
                                output_.sql_ += " IS NOT TRUE";
                                break;
                            case db_unary_operator::is_not_false:
                                output_.sql_ += " IS NOT FALSE";
                                break;
                            default:
                                std::terminate();
                        }
                }
                output_.sql_ += ')';
                break;
            }
            case db_node_kind::between:
                output_.sql_ += '(';
                child_value(n.args_[0]);
                output_.sql_ += n.flag_ ? " NOT BETWEEN " : " BETWEEN ";
                child_value(n.args_[1]);
                output_.sql_ += " AND ";
                child_value(n.args_[2]);
                output_.sql_ += ')';
                break;
            case db_node_kind::tuple:
            case db_node_kind::list:
            case db_node_kind::array:
                if (n.kind_ == db_node_kind::array) {
                    output_.require_pg("SQL arrays");
                    output_.sql_ += "ARRAY[";
                } else {
                    if (n.args_.empty()) {
                        throw std::invalid_argument("empty SQL tuple or list");
                    }
                    output_.sql_ += n.kind_ == db_node_kind::tuple ? "ROW(" : "(";
                }
                expressions(s, n.args_, depth + 1);
                output_.sql_ += n.kind_ == db_node_kind::array ? ']' : ')';
                break;
            case db_node_kind::any:
            case db_node_kind::all:
                output_.require_pg("array ANY/ALL");
                output_.sql_ += n.kind_ == db_node_kind::any ? "ANY(" : "ALL(";
                child_value(n.left_);
                output_.sql_ += ')';
                break;
            case db_node_kind::cast:
                output_.sql_ += "CAST(";
                child_value(n.left_);
                output_.sql_ += " AS ";
                output_.type(n.type_);
                output_.sql_ += ')';
                break;
            case db_node_kind::alias:
                if (!aliases) {
                    throw std::invalid_argument("an alias is only valid in a projection or RETURNING");
                }
                child_value(n.left_);
                output_.sql_ += " AS ";
                output_.identifier(n.text_);
                break;
            case db_node_kind::case_value:
                output_.sql_ += "CASE";
                for (std::size_t i = 0; i < n.args_.size(); i += 2) {
                    output_.sql_ += " WHEN ";
                    child_value(n.args_[i]);
                    output_.sql_ += " THEN ";
                    child_value(n.args_[i + 1]);
                }
                if (n.left_ != no_db_node) {
                    output_.sql_ += " ELSE ";
                    child_value(n.left_);
                }
                output_.sql_ += " END";
                break;
            case db_node_kind::exists:
            case db_node_kind::subquery:
                if (n.kind_ == db_node_kind::exists) {
                    output_.sql_ += "EXISTS ";
                }
                output_.sql_ += '(';
                query(nested(s, n.query_), depth + 1);
                output_.sql_ += ')';
                break;
            case db_node_kind::filter:
                output_.require_pg("aggregate FILTER");
                child_value(n.left_);
                output_.sql_ += " FILTER (WHERE ";
                child_value(n.right_);
                output_.sql_ += ')';
                break;
            case db_node_kind::window: {
                child_value(n.left_);
                output_.sql_ += " OVER (";
                bool space = false;
                if (!n.args_.empty()) {
                    output_.sql_ += "PARTITION BY ";
                    expressions(s, n.args_, depth + 1);
                    space = true;
                }
                if (!n.orders_.empty()) {
                    if (space) {
                        output_.sql_ += ' ';
                    }
                    output_.sql_ += "ORDER BY ";
                    orders(s, n.orders_, depth + 1);
                    space = true;
                }
                if (n.frame_) {
                    if (space) {
                        output_.sql_ += ' ';
                    }
                    const auto& frame = *n.frame_;
                    if (frame.start_.kind_ == db_frame_boundary::unbounded_following || frame.end_.kind_ == db_frame_boundary::unbounded_preceding || frame.start_.kind_ > frame.end_.kind_) {
                        throw std::invalid_argument("invalid window frame boundaries");
                    }
                    switch (frame.kind_) {
                        case db_window_frame::rows:
                            output_.sql_ += "ROWS";
                            break;
                        case db_window_frame::range:
                            output_.sql_ += "RANGE";
                            break;
                        case db_window_frame::groups:
                            output_.require_pg("GROUPS window frame");
                            output_.sql_ += "GROUPS";
                            break;
                    }
                    output_.sql_ += " BETWEEN ";
                    boundary(frame.start_);
                    output_.sql_ += " AND ";
                    boundary(frame.end_);
                }
                output_.sql_ += ')';
                break;
            }
            case db_node_kind::within_group:
                output_.require_pg("ordered-set aggregate");
                child_value(n.left_);
                output_.sql_ += " WITHIN GROUP (ORDER BY ";
                orders(s, n.orders_, depth + 1);
                output_.sql_ += ')';
                break;
            case db_node_kind::extract: {
                output_.sql_ += "EXTRACT(";
                switch (n.date_part_) {
                    case db_date_part::epoch:
                        output_.require_pg("EXTRACT EPOCH");
                        output_.sql_ += "EPOCH";
                        break;
                    case db_date_part::year:
                        output_.sql_ += "YEAR";
                        break;
                    case db_date_part::month:
                        output_.sql_ += "MONTH";
                        break;
                    case db_date_part::day:
                        output_.sql_ += "DAY";
                        break;
                    case db_date_part::hour:
                        output_.sql_ += "HOUR";
                        break;
                    case db_date_part::minute:
                        output_.sql_ += "MINUTE";
                        break;
                    case db_date_part::second:
                        output_.sql_ += "SECOND";
                        break;
                    case db_date_part::dow:
                        output_.require_pg("EXTRACT DOW");
                        output_.sql_ += "DOW";
                        break;
                    case db_date_part::doy:
                        output_.require_pg("EXTRACT DOY");
                        output_.sql_ += "DOY";
                        break;
                    case db_date_part::week:
                        output_.sql_ += "WEEK";
                        break;
                    case db_date_part::quarter:
                        output_.sql_ += "QUARTER";
                        break;
                }
                output_.sql_ += " FROM ";
                child_value(n.left_);
                output_.sql_ += ')';
                break;
            }
            case db_node_kind::subscript:
                output_.require_pg("array subscript");
                output_.sql_ += '(';
                child_value(n.left_);
                output_.sql_ += ")[";
                child_value(n.right_);
                output_.sql_ += ']';
                break;
            case db_node_kind::collate:
                output_.sql_ += '(';
                child_value(n.left_);
                output_.sql_ += " COLLATE ";
                output_.qualified(n.text_);
                output_.sql_ += ')';
                break;
        }
    }
    void binary(const db_query_storage& s, const db_query_node& n, std::size_t depth) {
        const auto& left = s.nodes_.at(n.left_);
        const auto& right = s.nodes_.at(n.right_);
        const bool null_left = left.kind_ == db_node_kind::value && db_value_access::type(left.value_) == db_value_type::null;
        const bool null_right = right.kind_ == db_node_kind::value && db_value_access::type(right.value_) == db_value_type::null;
        if ((n.binary_ == db_binary_operator::equal || n.binary_ == db_binary_operator::not_equal) && (null_left || null_right)) {
            output_.sql_ += '(';
            expr(s, null_left ? n.right_ : n.left_, depth);
            output_.sql_ += n.binary_ == db_binary_operator::equal ? " IS NULL)" : " IS NOT NULL)";
            return;
        }
        if ((n.binary_ == db_binary_operator::in || n.binary_ == db_binary_operator::not_in) && right.kind_ == db_node_kind::list && right.args_.empty()) {
            output_.sql_ += n.binary_ == db_binary_operator::in ? "FALSE" : "TRUE";
            return;
        }
        if (n.binary_ == db_binary_operator::concat && !output_.pg()) {
            output_.sql_ += "CONCAT(";
            expr(s, n.left_, depth);
            output_.sql_ += ", ";
            expr(s, n.right_, depth);
            output_.sql_ += ')';
            return;
        }
        const bool maria_distinct = !output_.pg() && n.binary_ == db_binary_operator::is_distinct_from;
        output_.sql_ += '(';
        if (maria_distinct) {
            output_.sql_ += "NOT (";
        }
        expr(s, n.left_, depth);
        std::string_view token;
        switch (n.binary_) {
            case db_binary_operator::equal:
                token = "=";
                break;
            case db_binary_operator::not_equal:
                token = "<>";
                break;
            case db_binary_operator::less:
                token = "<";
                break;
            case db_binary_operator::less_equal:
                token = "<=";
                break;
            case db_binary_operator::greater:
                token = ">";
                break;
            case db_binary_operator::greater_equal:
                token = ">=";
                break;
            case db_binary_operator::and_value:
                token = "AND";
                break;
            case db_binary_operator::or_value:
                token = "OR";
                break;
            case db_binary_operator::add:
                token = "+";
                break;
            case db_binary_operator::subtract:
                token = "-";
                break;
            case db_binary_operator::multiply:
                token = "*";
                break;
            case db_binary_operator::divide:
                token = "/";
                break;
            case db_binary_operator::modulo:
                token = "%";
                break;
            case db_binary_operator::concat:
                token = "||";
                break;
            case db_binary_operator::like:
                token = "LIKE";
                break;
            case db_binary_operator::not_like:
                token = "NOT LIKE";
                break;
            case db_binary_operator::i_like:
                output_.require_pg("ILIKE");
                token = "ILIKE";
                break;
            case db_binary_operator::not_i_like:
                output_.require_pg("NOT ILIKE");
                token = "NOT ILIKE";
                break;
            case db_binary_operator::in:
                token = "IN";
                break;
            case db_binary_operator::not_in:
                token = "NOT IN";
                break;
            case db_binary_operator::is_distinct_from:
                token = output_.pg() ? "IS DISTINCT FROM" : "<=>";
                break;
            case db_binary_operator::is_not_distinct_from:
                token = output_.pg() ? "IS NOT DISTINCT FROM" : "<=>";
                break;
            case db_binary_operator::bit_and:
                token = "&";
                break;
            case db_binary_operator::bit_or:
                token = "|";
                break;
            case db_binary_operator::bit_xor:
                token = output_.pg() ? "#" : "^";
                break;
            default:
                output_.require_pg("PostgreSQL JSON, array, regex or network operator");
                switch (n.binary_) {
                    case db_binary_operator::json_get:
                        token = "->";
                        break;
                    case db_binary_operator::json_get_text:
                        token = "->>";
                        break;
                    case db_binary_operator::json_path:
                        token = "#>";
                        break;
                    case db_binary_operator::json_path_text:
                        token = "#>>";
                        break;
                    case db_binary_operator::json_contains:
                    case db_binary_operator::array_contains:
                        token = "@>";
                        break;
                    case db_binary_operator::json_contained_by:
                    case db_binary_operator::array_contained_by:
                        token = "<@";
                        break;
                    case db_binary_operator::json_has_key:
                        token = "?";
                        break;
                    case db_binary_operator::json_has_any_key:
                        token = "?|";
                        break;
                    case db_binary_operator::json_has_all_keys:
                        token = "?&";
                        break;
                    case db_binary_operator::json_concat:
                        token = "||";
                        break;
                    case db_binary_operator::json_delete:
                        token = "-";
                        break;
                    case db_binary_operator::json_delete_path:
                        token = "#-";
                        break;
                    case db_binary_operator::array_overlap:
                    case db_binary_operator::inet_overlap:
                        token = "&&";
                        break;
                    case db_binary_operator::regex:
                        token = "~";
                        break;
                    case db_binary_operator::regex_insensitive:
                        token = "~*";
                        break;
                    case db_binary_operator::inet_contains:
                        token = ">>";
                        break;
                    case db_binary_operator::inet_contains_or_equal:
                        token = ">>=";
                        break;
                    case db_binary_operator::inet_contained_by:
                        token = "<<";
                        break;
                    case db_binary_operator::inet_contained_by_or_equal:
                        token = "<<=";
                        break;
                    default:
                        std::terminate();
                }
        }
        if ((n.binary_ == db_binary_operator::in || n.binary_ == db_binary_operator::not_in) && right.kind_ != db_node_kind::list && right.kind_ != db_node_kind::subquery) {
            throw std::invalid_argument("IN requires a list or a subquery");
        }
        output_.sql_ += ' ';
        output_.sql_ += token;
        output_.sql_ += ' ';
        expr(s, n.right_, depth);
        if (maria_distinct) {
            output_.sql_ += ')';
        }
        output_.sql_ += ')';
    }
    void source(const db_query_storage& s, const db_query_source& value, std::size_t depth) {
        if (value.lateral_) {
            output_.require_pg("LATERAL");
            output_.sql_ += "LATERAL ";
        }
        switch (value.kind_) {
            case db_source_kind::table:
                output_.qualified(value.name_);
                break;
            case db_source_kind::query:
                output_.sql_ += '(';
                query(nested(s, value.query_), depth + 1);
                output_.sql_ += ')';
                break;
            case db_source_kind::function:
                output_.require_pg("table function");
                expr(s, value.expression_, depth + 1);
                break;
        }
        if (value.ordinality_) {
            output_.require_pg("WITH ORDINALITY");
            if (value.kind_ != db_source_kind::function) {
                throw std::invalid_argument("WITH ORDINALITY requires a table function");
            }
            output_.sql_ += " WITH ORDINALITY";
        }
        if (!value.alias_.empty()) {
            output_.sql_ += " AS ";
            output_.identifier(value.alias_);
        }
        if (!value.columns_.empty()) {
            if (value.alias_.empty()) {
                throw std::invalid_argument("source column names require an alias");
            }
            output_.sql_ += " (";
            const bool typed = value.columns_.front().type_.data_type_ != db_data_type::inferred || !value.columns_.front().type_.custom_name_.empty();
            if (typed && (value.kind_ != db_source_kind::function || value.ordinality_)) {
                throw std::invalid_argument("record definitions require a function without ordinality");
            }
            separated(value.columns_, [&](const auto& column) {
                const bool has_type = column.type_.data_type_ != db_data_type::inferred || !column.type_.custom_name_.empty();
                if (has_type != typed) {
                    throw std::invalid_argument("source column definitions must all specify a type or all omit it");
                }
                output_.identifier(column.name_);
                if (typed) {
                    output_.sql_ += ' ';
                    const auto& t = column.type_;
                    append_db_type_name(output_.sql_, t.data_type_, t.custom_name_, t.length_, t.precision_, t.scale_, t.array_, output_.driver());
                }
            });
            output_.sql_ += ')';
        }
    }
    void sources(const db_query_storage& s, std::size_t depth) {
        source(s, *s.source_, depth);
        for (const auto& join : s.joins_) {
            switch (join.type_) {
                case db_join_type::inner:
                    output_.sql_ += " INNER JOIN ";
                    break;
                case db_join_type::left:
                    output_.sql_ += " LEFT JOIN ";
                    break;
                case db_join_type::right:
                    output_.sql_ += " RIGHT JOIN ";
                    break;
                case db_join_type::full:
                    output_.require_pg("FULL JOIN");
                    output_.sql_ += " FULL JOIN ";
                    break;
                case db_join_type::cross:
                    output_.sql_ += " CROSS JOIN ";
                    break;
            }
            if (join.type_ == db_join_type::cross) {
                if (join.on_ != no_db_node || !join.using_columns_.empty()) {
                    throw std::invalid_argument("CROSS JOIN has no ON or USING");
                }
            } else if ((join.on_ != no_db_node) == !join.using_columns_.empty()) {
                throw std::invalid_argument("JOIN requires exactly one of ON or USING");
            }
            source(s, join.source_, depth + 1);
            if (join.on_ != no_db_node) {
                output_.sql_ += " ON ";
                expr(s, join.on_, depth + 1);
            }
            if (!join.using_columns_.empty()) {
                output_.sql_ += " USING (";
                identifiers(join.using_columns_);
                output_.sql_ += ')';
            }
        }
    }
    void assignments(const db_query_storage& s, const std::pmr::vector<db_stored_assignment>& values, std::size_t depth) {
        if (values.empty()) {
            throw std::invalid_argument("UPDATE requires assignments");
        }
        separated(values, [&](const auto& assignment) { output_.identifier(assignment.column_); output_.sql_ += " = "; expr(s, assignment.expression_, depth + 1); });
    }
    void conflict(const db_query_storage& s, std::size_t depth) {
        const auto& value = *s.conflict_;
        if (value.do_nothing_ && (!value.assignments_.empty() || value.update_where_ != no_db_node)) {
            throw std::invalid_argument("DO NOTHING cannot specify updates");
        }
        if (output_.pg()) {
            if (value.any_unique_key_) {
                throw std::invalid_argument("any_unique_key is specific to MariaDB");
            }
            if (!value.columns_.empty() && !value.constraint_.empty()) {
                throw std::invalid_argument("conflict target cannot have both columns and a constraint");
            }
            if (value.target_where_ != no_db_node && value.columns_.empty()) {
                throw std::invalid_argument("a conflict predicate requires target columns");
            }
            output_.sql_ += " ON CONFLICT";
            if (!value.columns_.empty()) {
                output_.sql_ += " (";
                identifiers(value.columns_);
                output_.sql_ += ')';
            }
            if (!value.constraint_.empty()) {
                output_.sql_ += " ON CONSTRAINT ";
                output_.identifier(value.constraint_);
            }
            if (value.target_where_ != no_db_node) {
                output_.sql_ += " WHERE ";
                expr(s, value.target_where_, depth + 1);
            }
            if (value.do_nothing_) {
                output_.sql_ += " DO NOTHING";
                return;
            }
            if (value.columns_.empty() && value.constraint_.empty()) {
                throw std::invalid_argument("DO UPDATE requires a conflict target");
            }
            output_.sql_ += " DO UPDATE SET ";
            assignments(s, value.assignments_, depth);
            if (value.update_where_ != no_db_node) {
                output_.sql_ += " WHERE ";
                expr(s, value.update_where_, depth + 1);
            }
        } else {
            if (!value.any_unique_key_ || !value.columns_.empty() || !value.constraint_.empty() || value.target_where_ != no_db_node || value.update_where_ != no_db_node || value.do_nothing_) {
                output_.unsupported("targeted or conditional upsert / DO NOTHING");
            }
            output_.sql_ += " ON DUPLICATE KEY UPDATE ";
            assignments(s, value.assignments_, depth);
        }
    }
    void rows(const db_query_storage& s, std::size_t depth) {
        if (s.rows_.empty() || s.rows_.front().empty()) {
            throw std::invalid_argument("VALUES requires nonempty rows");
        }
        const auto width = s.rows_.front().size();
        if (!s.columns_.empty() && width != s.columns_.size()) {
            throw std::invalid_argument("INSERT column and row widths differ");
        }
        output_.sql_ += "VALUES ";
        separated(s.rows_, [&](const auto& row) {
            if (row.size() != width) {
                throw std::invalid_argument("VALUES rows have different widths");
            }
            output_.sql_ += '(';
            expressions(s, row, depth + 1);
            output_.sql_ += ')';
        });
    }
    void body(const db_query_storage& s, std::size_t depth) {
        switch (s.kind_) {
            case db_query_kind::select:
                output_.sql_ += "SELECT ";
                if (s.distinct_) {
                    output_.sql_ += "DISTINCT ";
                }
                if (!s.distinct_on_.empty()) {
                    output_.require_pg("DISTINCT ON");
                    output_.sql_ += "DISTINCT ON (";
                    expressions(s, s.distinct_on_, depth + 1);
                    output_.sql_ += ") ";
                }
                if (s.projections_.empty()) {
                    output_.sql_ += '*';
                } else {
                    expressions(s, s.projections_, depth + 1, true);
                }
                if (s.source_) {
                    output_.sql_ += " FROM ";
                    sources(s, depth + 1);
                }
                break;
            case db_query_kind::values:
                rows(s, depth);
                break;
            case db_query_kind::insert:
                output_.sql_ += "INSERT INTO ";
                output_.qualified(s.target_);
                if (!s.target_alias_.empty()) {
                    output_.require_pg("INSERT target alias");
                    output_.sql_ += " AS ";
                    output_.identifier(s.target_alias_);
                }
                if (!s.columns_.empty()) {
                    output_.sql_ += " (";
                    identifiers(s.columns_);
                    output_.sql_ += ')';
                }
                output_.sql_ += ' ';
                if (s.insert_query_ != no_db_node) {
                    if (!s.rows_.empty()) {
                        throw std::invalid_argument("INSERT cannot have both VALUES and SELECT");
                    }
                    query(nested(s, s.insert_query_), depth + 1);
                } else if (s.rows_.empty()) {
                    if (!s.columns_.empty()) {
                        throw std::invalid_argument("INSERT column list requires input rows");
                    }
                    output_.sql_ += output_.pg() ? "DEFAULT VALUES" : "() VALUES ()";
                } else {
                    rows(s, depth);
                }
                if (s.conflict_) {
                    conflict(s, depth + 1);
                }
                break;
            case db_query_kind::update:
            case db_query_kind::delete_value:
                output_.sql_ += s.kind_ == db_query_kind::update ? "UPDATE " : "DELETE FROM ";
                output_.qualified(s.target_);
                if (!s.target_alias_.empty()) {
                    if (!output_.pg() && s.kind_ == db_query_kind::delete_value) {
                        output_.unsupported("DELETE target alias");
                    }
                    output_.sql_ += " AS ";
                    output_.identifier(s.target_alias_);
                }
                if (s.kind_ == db_query_kind::update) {
                    output_.sql_ += " SET ";
                    assignments(s, s.assignments_, depth + 1);
                }
                if (s.source_) {
                    output_.require_pg("UPDATE FROM / DELETE USING");
                    output_.sql_ += s.kind_ == db_query_kind::update ? " FROM " : " USING ";
                    sources(s, depth + 1);
                }
                break;
        }
        if (s.predicate_ != no_db_node) {
            output_.sql_ += " WHERE ";
            expr(s, s.predicate_, depth + 1);
        }
        if (!s.groups_.empty()) {
            output_.sql_ += " GROUP BY ";
            expressions(s, s.groups_, depth + 1);
        }
        if (s.having_ != no_db_node) {
            output_.sql_ += " HAVING ";
            expr(s, s.having_, depth + 1);
        }
    }
};

}  // namespace ruvia::detail

namespace ruvia {

db_statement db_query::compile(db_driver driver, std::pmr::memory_resource* resource, db_parameter_mode mode) const {
    detail::db_query_compiler compiler(driver, detail::pmr_resource_or_default(resource), mode);
    compiler.query(storage());
    return db_statement(std::move(compiler.output_.sql_), std::move(compiler.output_.params_), returns_rows());
}

std::pmr::string db_query::render_expression(expr_type expression, db_driver driver,
    std::pmr::memory_resource* resource, db_parameter_mode mode) {
    if (mode != db_parameter_mode::literal) {
        throw std::invalid_argument("standalone expression rendering requires literal mode");
    }
    detail::db_query_compiler compiler(driver, detail::pmr_resource_or_default(resource), mode);
    compiler.expression(expression);
    return std::move(compiler.output_.sql_);
}

}  // namespace ruvia
