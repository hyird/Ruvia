#include "ruvia/web/db/db_query.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <stdexcept>

#include "ruvia/core/memory/pmr_resource.h"

#include "db/db_sql_format.h"
#include "query_storage.h"

namespace ruvia::detail {

namespace {
void require_name(std::string_view name, bool qualified = false) {
    if (name.empty() || name.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("database identifier must be nonempty and contain no NUL");
    }
    if (qualified && (name.front() == '.' || name.back() == '.' || name.find("..") != std::string_view::npos)) {
        throw std::invalid_argument("database qualified identifier has an empty component");
    }
}
void set_source_options(db_query_source& source_value, const db_source_options& options, std::pmr::memory_resource* resource) {
    source_value.lateral_ = options.lateral_;
    source_value.ordinality_ = options.with_ordinality_;
    for (const auto& column : options.columns_) {
        require_name(column.name_);
        source_value.columns_.emplace_back(column, resource);
    }
}
}  // namespace

}  // namespace ruvia::detail

namespace ruvia {

using detail::db_node_kind;
using detail::db_query_kind;
using detail::db_source_kind;
using detail::no_db_node;

void db_query::storage_deleter_type::operator()(detail::db_query_storage* storage) const noexcept {
    if (storage != nullptr) {
        std::destroy_at(storage);
        resource_->deallocate(storage, sizeof(detail::db_query_storage), alignof(detail::db_query_storage));
    }
}

db_query::db_query(std::pmr::memory_resource* resource)
    : storage_(nullptr, storage_deleter_type{detail::pmr_resource_or_default(resource)}) {
    auto* resolved = storage_.get_deleter().resource_;
    void* block = resolved->allocate(sizeof(detail::db_query_storage), alignof(detail::db_query_storage));
    try {
        storage_.reset(std::construct_at(static_cast<detail::db_query_storage*>(block), resolved));
    } catch (...) {
        resolved->deallocate(block, sizeof(detail::db_query_storage), alignof(detail::db_query_storage));
        throw;
    }
}
db_query::db_query(storage_owner_type storage) noexcept
    : storage_(std::move(storage)) {}
db_query::db_query(db_query&&) noexcept = default;
db_query& db_query::operator=(db_query&&) noexcept = default;
db_query::~db_query() = default;

detail::db_query_storage& db_query::storage() {
    if (!storage_) {
        throw std::logic_error("database query was moved from");
    }
    return *storage_;
}
const detail::db_query_storage& db_query::storage() const {
    if (!storage_) {
        throw std::logic_error("database query was moved from");
    }
    return *storage_;
}
std::pmr::memory_resource* db_query::resource() const {
    return storage().resource_;
}
bool db_query::returns_rows() const {
    return storage().returns_rows();
}
void db_query::require_select_query() const {
    if (storage().kind_ != db_query_kind::select && storage().kind_ != db_query_kind::values) {
        throw std::invalid_argument("subqueries require SELECT or VALUES; use a CTE for DML RETURNING");
    }
    for (const auto& cte : storage().ctes_) {
        storage().queries_.at(cte.query_).require_select_query();
    }
}
bool db_query::has_writes() const {
    const auto& s = storage();
    return (s.kind_ != db_query_kind::select && s.kind_ != db_query_kind::values) ||
           std::ranges::any_of(s.queries_, [](const auto& query) { return query.has_writes(); });
}
bool db_query::has_where() const {
    return storage().predicate_ != no_db_node;
}
bool db_query::has_grouping() const {
    return !storage().groups_.empty() || storage().having_ != no_db_node;
}
bool db_query::uses_source_name(std::string_view name) const {
    const auto matches = [name](const detail::db_query_source& source_value) {
        if (!source_value.alias_.empty()) {
            return source_value.alias_ == name;
        }
        const auto full = std::string_view(source_value.name_);
        const auto dot = full.rfind('.');
        return full == name || full.substr(dot == std::string_view::npos ? 0 : dot + 1) == name;
    };
    const auto& s = storage();
    return (s.source_ && matches(*s.source_)) || std::ranges::any_of(s.joins_,
                                                     [&](const auto& join) { return matches(join.source_); });
}
bool db_query::uses_projection_name(std::string_view name) const {
    const auto& s = storage();
    return std::ranges::any_of(s.projections_, [&](auto index) {
        const auto& node_value = s.nodes_[index];
        return (node_value.kind_ == db_node_kind::column || node_value.kind_ == db_node_kind::alias) && node_value.text_ == name;
    });
}

std::optional<db_query> db_query::prepare_entity_read(
    std::span<const std::string_view> primary_key, std::string_view root_alias,
    db_driver driver) const {
    const auto& s = storage();
    if (s.kind_ != db_query_kind::select || primary_key.empty()) {
        throw std::invalid_argument("relation loading requires an entity SELECT with a primary key");
    }
    const bool paginated = s.limit_.has_value() || s.offset_.has_value();
    const bool scope_lock = driver == db_driver::postgresql && s.lock_ && s.lock_->tables_.empty();
    if (!paginated && !scope_lock) {
        return std::nullopt;
    }
    auto result_value = clone(s.resource_);
    if (scope_lock) {
        result_value.storage().lock_->tables_.emplace_back(root_alias);
    }
    if (!paginated) {
        return result_value;
    }
    if (has_grouping() || !s.set_operations_.empty() || !s.distinct_on_.empty()) {
        throw std::invalid_argument("paged relation loading requires an ungrouped SELECT without set operations or DISTINCT ON");
    }
    if (s.lock_ && s.lock_->skip_locked_) {
        throw std::invalid_argument("paged relation loading cannot combine SKIP LOCKED with collection expansion");
    }

    // Select a page of root identities before filtering the expanded result.
    // Ranking makes joined ordering deterministic for each root, and avoids
    // applying LIMIT to child rows or truncating a loaded collection.
    auto ranked = clone(s.resource_);
    auto& rank_storage = ranked.storage();
    // Refer to the outer WITH bindings. Copying their declarations into the
    // page would evaluate volatile CTEs twice and relocate DML CTEs illegally.
    rank_storage.ctes_.clear();
    auto orders = std::move(rank_storage.orders_);
    rank_storage.limit_.reset();
    rank_storage.offset_.reset();
    rank_storage.lock_.reset();
    rank_storage.distinct_ = false;
    for (auto& order : orders) {
        const auto& node_value = rank_storage.nodes_[order.expression_];
        if (node_value.kind_ == db_node_kind::column && node_value.qualifier_.empty()) {
            for (const auto index : rank_storage.projections_) {
                const auto& projection = rank_storage.nodes_[index];
                if (projection.kind_ == db_node_kind::alias && projection.text_ == node_value.text_) {
                    order.expression_ = projection.left_;
                    break;
                }
            }
        }
    }
    rank_storage.projections_.clear();
    detail::db_query_node window(db_node_kind::window, s.resource_);
    window.left_ = ranked.require_expression(ranked.call("row_number"));
    std::pmr::vector<std::pmr::string> key_names(s.resource_), order_names(s.resource_);
    for (std::size_t i = 0; i < primary_key.size(); ++i) {
        auto& name = key_names.emplace_back("__ruvia_page_key_");
        detail::append_db_number(name, static_cast<std::uint64_t>(i));
        const auto key = ranked.column(primary_key[i], root_alias);
        window.args_.push_back(ranked.require_expression(key));
        ranked.add_select(ranked.alias(key, name));
    }
    if (orders.empty()) {
        for (const auto key : window.args_) {
            orders.push_back({key, db_order_direction::asc, db_nulls_order::default_value});
        }
    }
    // Give the outer result the same stable tie order as its page.
    for (const auto key : primary_key) {
        result_value.add_order_by(result_value.column(key, root_alias));
    }
    for (std::size_t i = 0; i < orders.size(); ++i) {
        auto& name = order_names.emplace_back("__ruvia_page_order_");
        detail::append_db_number(name, static_cast<std::uint64_t>(i));
        ranked.add_select(ranked.alias(expr_type(&rank_storage, orders[i].expression_), name));
    }
    window.orders_ = orders;
    rank_storage.nodes_.push_back(std::move(window));
    ranked.add_select(ranked.alias(expr_type(&rank_storage, rank_storage.nodes_.size() - 1), "__ruvia_page_rank"));

    db_query page(s.resource_);
    for (const auto& key : key_names) {
        page.add_select(page.column(key, "__ruvia_ranked"));
    }
    page.from(ranked, "__ruvia_ranked");
    page.where(page.binary(page.column("__ruvia_page_rank", "__ruvia_ranked"),
        db_binary_operator::equal, page.value(1)));
    for (std::size_t i = 0; i < orders.size(); ++i) {
        page.add_order_by(page.column(order_names[i], "__ruvia_ranked"), orders[i].direction_, orders[i].nulls_);
    }
    // Break ordering ties using the complete primary key, including composite keys.
    for (const auto& key : key_names) {
        page.add_order_by(page.column(key, "__ruvia_ranked"));
    }
    page.limit(s.limit_).offset(s.offset_);
    db_query included(s.resource_);
    const auto page_alias = root_alias == "__ruvia_page" ? "__ruvia_page_1" : "__ruvia_page";
    included.select(included.value(1)).from(page, page_alias);
    for (std::size_t i = 0; i < primary_key.size(); ++i) {
        included.and_where(included.binary(included.column(key_names[i], page_alias),
            db_binary_operator::equal, included.column(primary_key[i], root_alias)));
    }
    result_value.limit(std::nullopt).offset(std::nullopt);
    result_value.and_where(result_value.exists(included));
    return result_value;
}
std::size_t db_query::require_expression(expr_type expression) const {
    if (expression.owner_ != storage_.get() || expression.node_ >= storage().nodes_.size()) {
        throw std::invalid_argument("database expression belongs to another query; import it first");
    }
    return expression.node_;
}

db_query::storage_owner_type db_query::copy_storage(const detail::db_query_storage& source_value, std::pmr::memory_resource* resource) {
    db_query copy(resource);
    auto& target = copy.storage();
    target.cache_enabled_ = source_value.cache_enabled_;
    target.cache_duration_ = source_value.cache_duration_;
    target.cache_id_ = source_value.cache_id_;
    target.kind_ = source_value.kind_;
    target.target_ = source_value.target_;
    target.target_alias_ = source_value.target_alias_;
    target.columns_ = source_value.columns_;
    target.projections_ = source_value.projections_;
    target.groups_ = source_value.groups_;
    target.orders_ = source_value.orders_;
    target.distinct_ = source_value.distinct_;
    target.distinct_on_ = source_value.distinct_on_;
    target.returning_ = source_value.returning_;
    target.rows_ = source_value.rows_;
    target.insert_query_ = source_value.insert_query_;
    target.predicate_ = source_value.predicate_;
    target.having_ = source_value.having_;
    target.limit_ = source_value.limit_;
    target.offset_ = source_value.offset_;
    target.set_operations_ = source_value.set_operations_;
    target.nodes_.reserve(source_value.nodes_.size());
    for (const auto& node : source_value.nodes_) {
        target.nodes_.emplace_back(node, target.resource_);
    }
    target.queries_.reserve(source_value.queries_.size());
    for (const auto& query : source_value.queries_) {
        target.queries_.push_back(query.clone(target.resource_));
    }
    if (source_value.source_) {
        target.source_.emplace(*source_value.source_, target.resource_);
    }
    for (const auto& join : source_value.joins_) {
        target.joins_.emplace_back(join, target.resource_);
    }
    for (const auto& assignment : source_value.assignments_) {
        target.assignments_.emplace_back(assignment.column_, assignment.expression_, target.resource_);
    }
    for (const auto& cte : source_value.ctes_) {
        target.ctes_.emplace_back(cte, target.resource_);
    }
    if (source_value.conflict_) {
        target.conflict_.emplace(*source_value.conflict_, target.resource_);
    }
    if (source_value.lock_) {
        target.lock_.emplace(*source_value.lock_, target.resource_);
    }
    return std::move(copy.storage_);
}
db_query db_query::clone(std::pmr::memory_resource* resource) const {
    return db_query(copy_storage(storage(), detail::pmr_resource_or_default(resource)));
}

bool db_query::cacheable() const {
    const auto& s = storage();
    return (s.kind_ == db_query_kind::select || s.kind_ == db_query_kind::values) && !s.lock_ &&
           std::ranges::all_of(s.queries_, [](const auto& query) { return query.cacheable(); });
}
std::optional<bool> db_query::cache_enabled() const {
    return storage().cache_enabled_;
}
std::optional<std::chrono::milliseconds> db_query::cache_duration() const {
    return storage().cache_duration_;
}
std::string_view db_query::cache_id() const {
    return storage().cache_id_;
}
void db_query::copy_cache(const db_query& source_value, std::string_view suffix) {
    auto& s = storage();
    s.cache_enabled_ = source_value.cache_enabled();
    s.cache_duration_ = source_value.cache_duration();
    s.cache_id_ = source_value.cache_id();
    if (!s.cache_id_.empty()) {
        s.cache_id_.append(suffix);
    }
}
db_query& db_query::cache(const db_cache_setting_type& setting) {
    return std::visit([&](const auto& value) -> db_query& {
        using t_type = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<t_type, db_cache_options>) {
            return cache(std::string_view(value.id_), value.milliseconds_);
        } else if constexpr (std::same_as<t_type, std::chrono::milliseconds>) {
            return cache(std::string_view{}, value);
        } else {
            auto& s = storage();
            s.cache_enabled_.reset();
            if constexpr (std::same_as<t_type, bool>) {
                s.cache_enabled_ = value;
            }
            s.cache_duration_.reset();
            s.cache_id_.clear();
            return *this;
        }
    },
        setting);
}
db_query& db_query::cache(std::string_view id, std::optional<std::chrono::milliseconds> milliseconds) {
    if (milliseconds && milliseconds->count() <= 0) {
        throw std::invalid_argument("cache duration must be positive");
    }
    auto& s = storage();
    s.cache_id_ = id;
    s.cache_duration_ = milliseconds;
    s.cache_enabled_ = true;
    return *this;
}

db_query::expr_type db_query::column(std::string_view name, std::string_view table_value) {
    detail::require_name(name);
    if (!table_value.empty()) {
        detail::require_name(table_value, true);
    }
    auto& s = storage();
    detail::db_query_node node_value(db_node_kind::column, s.resource_);
    node_value.text_ = name;
    node_value.qualifier_ = table_value;
    s.nodes_.push_back(std::move(node_value));
    return expr_type(&s, s.nodes_.size() - 1);
}
db_query::expr_type db_query::star(std::string_view table_value) {
    if (!table_value.empty()) {
        detail::require_name(table_value, true);
    }
    auto& s = storage();
    detail::db_query_node node_value(db_node_kind::star, s.resource_);
    node_value.qualifier_ = table_value;
    s.nodes_.push_back(std::move(node_value));
    return expr_type(&s, s.nodes_.size() - 1);
}
db_query::expr_type db_query::value(const db_value& value) {
    auto& s = storage();
    detail::db_query_node node_value(db_node_kind::value, s.resource_);
    auto owned = detail::clone_db_value_for_resource(value, s.resource_);
    std::destroy_at(&node_value.value_);
    std::construct_at(&node_value.value_, std::move(owned));
    s.nodes_.push_back(std::move(node_value));
    return expr_type(&s, s.nodes_.size() - 1);
}
db_query::expr_type db_query::null_value() {
    return value(db_value(nullptr));
}
db_query::expr_type db_query::excluded(std::string_view column) {
    auto result_value = this->column(column);
    storage().nodes_[result_value.node_].kind_ = db_node_kind::excluded;
    return result_value;
}
db_query::expr_type db_query::default_value() {
    auto& s = storage();
    s.nodes_.emplace_back(db_node_kind::default_value, s.resource_);
    return expr_type(&s, s.nodes_.size() - 1);
}
db_query::expr_type db_query::sql(std::span<const std::string_view> parts, std::span<const expr_type> args) {
    if (parts.size() != args.size() + 1 || (args.empty() && parts.front().empty())) {
        throw std::invalid_argument("SQL expression requires one more syntax part than arguments");
    }
    auto& s = storage();
    detail::db_query_node node_value(db_node_kind::sql, s.resource_);
    for (auto part : parts) {
        if (part.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("SQL expression cannot contain NUL");
        }
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
        node_value.named_.emplace_back(parts[i], require_expression(args[i]), s.resource_);
    }
    node_value.text_ = parts.back();
    s.nodes_.push_back(std::move(node_value));
    return expr_type(&s, s.nodes_.size() - 1);
}
db_query::expr_type db_query::call(std::string_view function, std::span<const expr_type> args, std::span<const db_named_argument> named) {
    detail::require_name(function, true);
    auto& s = storage();
    detail::db_query_node node_value(db_node_kind::function, s.resource_);
    node_value.text_ = function;
    for (const auto arg : args) {
        node_value.args_.push_back(require_expression(arg));
    }
    for (const auto& arg : named) {
        detail::require_name(arg.name_);
        if (std::ranges::any_of(node_value.named_, [&](const auto& existing) { return std::string_view(existing.name_) == arg.name_; })) {
            throw std::invalid_argument("duplicate named database function argument");
        }
        node_value.named_.emplace_back(arg.name_, require_expression(arg.value_), s.resource_);
    }
    s.nodes_.push_back(std::move(node_value));
    return expr_type(&s, s.nodes_.size() - 1);
}
db_query::expr_type db_query::coalesce(std::span<const expr_type> args) {
    if (args.empty()) {
        throw std::invalid_argument("COALESCE requires an argument");
    }
    auto result_value = call("coalesce", args);
    storage().nodes_[result_value.node_].kind_ = db_node_kind::coalesce;
    return result_value;
}
db_query::expr_type db_query::null_if(expr_type value, expr_type other) {
    const std::array args{value, other};
    auto result_value = call("nullif", args);
    storage().nodes_[result_value.node_].kind_ = db_node_kind::null_if;
    return result_value;
}
db_query::expr_type db_query::greatest(std::span<const expr_type> args) {
    if (args.empty()) {
        throw std::invalid_argument("GREATEST requires an argument");
    }
    auto result_value = call("greatest", args);
    storage().nodes_[result_value.node_].kind_ = db_node_kind::greatest;
    return result_value;
}
db_query::expr_type db_query::least(std::span<const expr_type> args) {
    if (args.empty()) {
        throw std::invalid_argument("LEAST requires an argument");
    }
    auto result_value = call("least", args);
    storage().nodes_[result_value.node_].kind_ = db_node_kind::least;
    return result_value;
}
db_query::expr_type db_query::binary(expr_type lhs, db_binary_operator op, expr_type rhs) {
    auto& s = storage();
    const auto left = require_expression(lhs), right = require_expression(rhs);
    detail::db_query_node node_value(db_node_kind::binary, s.resource_);
    node_value.left_ = left;
    node_value.right_ = right;
    node_value.binary_ = op;
    s.nodes_.push_back(std::move(node_value));
    return expr_type(&s, s.nodes_.size() - 1);
}
db_query::expr_type db_query::unary(db_unary_operator op, expr_type value) {
    auto& s = storage();
    const auto child_value = require_expression(value);
    detail::db_query_node node_value(db_node_kind::unary, s.resource_);
    node_value.left_ = child_value;
    node_value.unary_ = op;
    s.nodes_.push_back(std::move(node_value));
    return expr_type(&s, s.nodes_.size() - 1);
}
db_query::expr_type db_query::between(expr_type value, expr_type lower, expr_type upper, bool negate) {
    auto& s = storage();
    detail::db_query_node node_value(db_node_kind::between, s.resource_);
    node_value.args_ = {require_expression(value), require_expression(lower), require_expression(upper)};
    node_value.flag_ = negate;
    s.nodes_.push_back(std::move(node_value));
    return expr_type(&s, s.nodes_.size() - 1);
}
db_query::expr_type db_query::tuple(std::span<const expr_type> values) {
    auto& s = storage();
    detail::db_query_node node_value(db_node_kind::tuple, s.resource_);
    for (const auto value : values) {
        node_value.args_.push_back(require_expression(value));
    }
    s.nodes_.push_back(std::move(node_value));
    return expr_type(&s, s.nodes_.size() - 1);
}
db_query::expr_type db_query::list(std::span<const expr_type> values) {
    auto result_value = tuple(values);
    storage().nodes_[result_value.node_].kind_ = db_node_kind::list;
    return result_value;
}
db_query::expr_type db_query::array(std::span<const expr_type> values) {
    auto result_value = tuple(values);
    storage().nodes_[result_value.node_].kind_ = db_node_kind::array;
    return result_value;
}
db_query::expr_type db_query::any(expr_type array_value) {
    auto result_value = unary(db_unary_operator::not_value, array_value);
    storage().nodes_[result_value.node_].kind_ = db_node_kind::any;
    return result_value;
}
db_query::expr_type db_query::all(expr_type array_value) {
    auto result_value = any(array_value);
    storage().nodes_[result_value.node_].kind_ = db_node_kind::all;
    return result_value;
}
db_query::expr_type db_query::cast(expr_type value, const db_type_definition& type) {
    const auto child_value = require_expression(value);
    auto& s = storage();
    detail::db_query_node node_value(db_node_kind::cast, s.resource_);
    node_value.left_ = child_value;
    node_value.type_.data_type_ = type.data_type_;
    node_value.type_.custom_name_ = type.custom_name_;
    node_value.type_.length_ = type.length_;
    node_value.type_.precision_ = type.precision_;
    node_value.type_.scale_ = type.scale_;
    node_value.type_.array_ = type.array_;
    s.nodes_.push_back(std::move(node_value));
    return expr_type(&s, s.nodes_.size() - 1);
}
db_query::expr_type db_query::alias(expr_type value, std::string_view name) {
    detail::require_name(name);
    const auto child_value = require_expression(value);
    auto& s = storage();
    detail::db_query_node node_value(db_node_kind::alias, s.resource_);
    node_value.left_ = child_value;
    node_value.text_ = name;
    s.nodes_.push_back(std::move(node_value));
    return expr_type(&s, s.nodes_.size() - 1);
}
db_query::expr_type db_query::case_when(std::span<const db_case_branch> branches, expr_type otherwise) {
    if (branches.empty()) {
        throw std::invalid_argument("CASE requires a branch");
    }
    auto& s = storage();
    detail::db_query_node node_value(db_node_kind::case_value, s.resource_);
    for (const auto& branch : branches) {
        node_value.args_.push_back(require_expression(branch.when_));
        node_value.args_.push_back(require_expression(branch.then_));
    }
    if (!otherwise.empty()) {
        node_value.left_ = require_expression(otherwise);
    }
    s.nodes_.push_back(std::move(node_value));
    return expr_type(&s, s.nodes_.size() - 1);
}
db_query::expr_type db_query::subquery(const db_query& query) {
    query.require_select_query();
    auto snapshot = query.clone(resource());
    auto& s = storage();
    const auto index = s.queries_.size();
    s.queries_.push_back(std::move(snapshot));
    detail::db_query_node node_value(db_node_kind::subquery, s.resource_);
    node_value.query_ = index;
    s.nodes_.push_back(std::move(node_value));
    return expr_type(&s, s.nodes_.size() - 1);
}
db_query::expr_type db_query::exists(const db_query& query) {
    auto result_value = subquery(query);
    storage().nodes_[result_value.node_].kind_ = db_node_kind::exists;
    return result_value;
}
db_query::expr_type db_query::aggregate(std::string_view function, std::span<const expr_type> args, bool distinct, std::span<const db_order_term> order) {
    auto result_value = call(function, args);
    auto& node_value = storage().nodes_[result_value.node_];
    node_value.kind_ = db_node_kind::aggregate;
    node_value.flag_ = distinct;
    for (const auto& term : order) {
        node_value.orders_.push_back({require_expression(term.expression_), term.direction_, term.nulls_});
    }
    return result_value;
}
db_query::expr_type db_query::filter(expr_type aggregate, expr_type predicate) {
    auto result_value = binary(aggregate, db_binary_operator::and_value, predicate);
    storage().nodes_[result_value.node_].kind_ = db_node_kind::filter;
    return result_value;
}
db_query::expr_type db_query::over(expr_type function, const db_window_options& window) {
    auto& s = storage();
    detail::db_query_node node_value(db_node_kind::window, s.resource_);
    node_value.left_ = require_expression(function);
    for (const auto expression : window.partition_by_) {
        node_value.args_.push_back(require_expression(expression));
    }
    for (const auto& term : window.order_by_) {
        node_value.orders_.push_back({require_expression(term.expression_), term.direction_, term.nulls_});
    }
    node_value.frame_ = window.frame_;
    s.nodes_.push_back(std::move(node_value));
    return expr_type(&s, s.nodes_.size() - 1);
}
db_query::expr_type db_query::within_group(expr_type function, std::span<const db_order_term> order) {
    if (order.empty()) {
        throw std::invalid_argument("WITHIN GROUP requires ordering");
    }
    auto& s = storage();
    detail::db_query_node node_value(db_node_kind::within_group, s.resource_);
    node_value.left_ = require_expression(function);
    for (const auto& term : order) {
        node_value.orders_.push_back({require_expression(term.expression_), term.direction_, term.nulls_});
    }
    s.nodes_.push_back(std::move(node_value));
    return expr_type(&s, s.nodes_.size() - 1);
}
db_query::expr_type db_query::extract(db_date_part part, expr_type value) {
    auto result_value = unary(db_unary_operator::not_value, value);
    auto& node_value = storage().nodes_[result_value.node_];
    node_value.kind_ = db_node_kind::extract;
    node_value.date_part_ = part;
    return result_value;
}
db_query::expr_type db_query::subscript(expr_type array_value, expr_type index) {
    auto result_value = binary(array_value, db_binary_operator::add, index);
    storage().nodes_[result_value.node_].kind_ = db_node_kind::subscript;
    return result_value;
}
db_query::expr_type db_query::collate(expr_type value, std::string_view collation) {
    auto result_value = alias(value, collation);
    storage().nodes_[result_value.node_].kind_ = db_node_kind::collate;
    return result_value;
}

db_query::expr_type db_query::import_expression(expr_type expression, std::string_view source_qualifier, std::string_view target_qualifier) {
    if (expression.empty() || expression.node_ >= expression.owner_->nodes_.size()) {
        throw std::invalid_argument("cannot import an empty database expression");
    }
    auto& target = storage();
    if (source_qualifier.empty() != target_qualifier.empty()) {
        throw std::invalid_argument("expression qualifier mapping requires both source and target");
    }
    if (!source_qualifier.empty()) {
        detail::require_name(source_qualifier, true);
        detail::require_name(target_qualifier, true);
    }
    if (expression.owner_ == &target && source_qualifier.empty()) {
        return expression;
    }
    const auto& source_value = *expression.owner_;
    std::pmr::vector<std::size_t> mapped(source_value.nodes_.size(), no_db_node, target.resource_);
    auto import = [&](auto&& self, std::size_t id, std::size_t depth) -> std::size_t {
        if (depth > 256) {
            throw std::length_error("database expression nesting exceeds 256 levels");
        }
        if (mapped[id] != no_db_node) {
            return mapped[id];
        }
        detail::db_query_node node_value(source_value.nodes_[id], target.resource_);
        if (!source_qualifier.empty() && (node_value.kind_ == db_node_kind::column || node_value.kind_ == db_node_kind::star) && node_value.qualifier_ == source_qualifier) {
            node_value.qualifier_ = target_qualifier;
        }
        if (node_value.left_ != no_db_node) {
            node_value.left_ = self(self, node_value.left_, depth + 1);
        }
        if (node_value.right_ != no_db_node) {
            node_value.right_ = self(self, node_value.right_, depth + 1);
        }
        for (auto& arg : node_value.args_) {
            arg = self(self, arg, depth + 1);
        }
        for (auto& arg : node_value.named_) {
            arg.expression_ = self(self, arg.expression_, depth + 1);
        }
        for (auto& order : node_value.orders_) {
            order.expression_ = self(self, order.expression_, depth + 1);
        }
        if (node_value.query_ != no_db_node) {
            auto snapshot = source_value.queries_[node_value.query_].clone(target.resource_);
            node_value.query_ = target.queries_.size();
            target.queries_.push_back(std::move(snapshot));
        }
        const auto index = target.nodes_.size();
        target.nodes_.push_back(std::move(node_value));
        mapped[id] = index;
        return index;
    };
    return expr_type(&target, import(import, expression.node_, 0));
}

db_query& db_query::select(std::span<const expr_type> projections) {
    auto& s = storage();
    std::pmr::vector<std::size_t> owned(s.resource_);
    for (const auto expression : projections) {
        owned.push_back(require_expression(expression));
    }
    s.projections_ = std::move(owned);
    return *this;
}
db_query& db_query::add_select(expr_type projection) {
    storage().projections_.push_back(require_expression(projection));
    return *this;
}
db_query& db_query::from(std::string_view table_value, std::string_view alias) {
    detail::require_name(table_value, true);
    if (!alias.empty()) {
        detail::require_name(alias);
    }
    auto& s = storage();
    detail::db_query_source source_value(s.resource_);
    source_value.name_ = table_value;
    source_value.alias_ = alias;
    s.source_ = std::move(source_value);
    return *this;
}
db_query& db_query::from(const db_query& query, std::string_view alias, const db_source_options& options) {
    detail::require_name(alias);
    query.require_select_query();
    auto snapshot = query.clone(resource());
    auto& s = storage();
    detail::db_query_source source_value(s.resource_);
    source_value.kind_ = db_source_kind::query;
    source_value.alias_ = alias;
    source_value.query_ = s.queries_.size();
    detail::set_source_options(source_value, options, s.resource_);
    s.queries_.push_back(std::move(snapshot));
    s.source_ = std::move(source_value);
    return *this;
}
db_query& db_query::from_function(expr_type function, std::string_view alias, const db_source_options& options) {
    detail::require_name(alias);
    const auto index = require_expression(function);
    auto& s = storage();
    detail::db_query_source source_value(s.resource_);
    source_value.kind_ = db_source_kind::function;
    source_value.expression_ = index;
    source_value.alias_ = alias;
    detail::set_source_options(source_value, options, s.resource_);
    s.source_ = std::move(source_value);
    return *this;
}
db_query& db_query::join(db_join_type type, std::string_view table_value, expr_type on, std::string_view alias, std::span<const std::string_view> using_columns) {
    detail::require_name(table_value, true);
    if (!alias.empty()) {
        detail::require_name(alias);
    }
    auto& s = storage();
    detail::db_query_source source_value(s.resource_);
    source_value.name_ = table_value;
    source_value.alias_ = alias;
    detail::db_stored_join join(type, std::move(source_value), on.empty() ? no_db_node : require_expression(on), s.resource_);
    for (const auto name : using_columns) {
        detail::require_name(name);
        join.using_columns_.emplace_back(name);
    }
    s.joins_.push_back(std::move(join));
    return *this;
}
db_query& db_query::join(db_join_type type, const db_query& query, expr_type on, std::string_view alias, const db_source_options& options) {
    detail::require_name(alias);
    const auto predicate = on.empty() ? no_db_node : require_expression(on);
    query.require_select_query();
    auto snapshot = query.clone(resource());
    auto& s = storage();
    detail::db_query_source source_value(s.resource_);
    source_value.kind_ = db_source_kind::query;
    source_value.query_ = s.queries_.size();
    source_value.alias_ = alias;
    detail::set_source_options(source_value, options, s.resource_);
    s.queries_.push_back(std::move(snapshot));
    s.joins_.emplace_back(type, std::move(source_value), predicate, s.resource_);
    return *this;
}
db_query& db_query::join_function(db_join_type type, expr_type function, expr_type on, std::string_view alias, const db_source_options& options) {
    detail::require_name(alias);
    const auto function_id = require_expression(function);
    const auto predicate = on.empty() ? no_db_node : require_expression(on);
    auto& s = storage();
    detail::db_query_source source_value(s.resource_);
    source_value.kind_ = db_source_kind::function;
    source_value.expression_ = function_id;
    source_value.alias_ = alias;
    detail::set_source_options(source_value, options, s.resource_);
    s.joins_.emplace_back(type, std::move(source_value), predicate, s.resource_);
    return *this;
}
db_query& db_query::where(expr_type predicate) {
    storage().predicate_ = require_expression(predicate);
    return *this;
}
db_query& db_query::and_where(expr_type predicate) {
    const auto previous = storage().predicate_;
    return where(previous == no_db_node ? predicate : binary(expr_type(storage_.get(), previous), db_binary_operator::and_value, predicate));
}
db_query& db_query::or_where(expr_type predicate) {
    const auto previous = storage().predicate_;
    return where(previous == no_db_node ? predicate : binary(expr_type(storage_.get(), previous), db_binary_operator::or_value, predicate));
}
db_query& db_query::group_by(std::span<const expr_type> expressions) {
    std::pmr::vector<std::size_t> groups(resource());
    for (const auto expression : expressions) {
        groups.push_back(require_expression(expression));
    }
    storage().groups_ = std::move(groups);
    return *this;
}
db_query& db_query::add_group_by(expr_type expression) {
    storage().groups_.push_back(require_expression(expression));
    return *this;
}
db_query& db_query::having(expr_type predicate) {
    storage().having_ = require_expression(predicate);
    return *this;
}
db_query& db_query::and_having(expr_type predicate) {
    const auto previous = storage().having_;
    return having(previous == no_db_node ? predicate : binary(expr_type(storage_.get(), previous), db_binary_operator::and_value, predicate));
}
db_query& db_query::order_by(expr_type expression, db_order_direction direction, db_nulls_order nulls) {
    const auto index = require_expression(expression);
    auto& s = storage();
    s.orders_.clear();
    s.orders_.push_back({index, direction, nulls});
    return *this;
}
db_query& db_query::add_order_by(expr_type expression, db_order_direction direction, db_nulls_order nulls) {
    storage().orders_.push_back({require_expression(expression), direction, nulls});
    return *this;
}
db_query& db_query::clear_order() {
    storage().orders_.clear();
    return *this;
}
db_query& db_query::limit(std::optional<std::uint64_t> count) {
    if (count && *count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("database limit exceeds signed 64-bit range");
    }
    storage().limit_ = count;
    return *this;
}
db_query& db_query::offset(std::optional<std::uint64_t> count) {
    if (count && *count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("database offset exceeds signed 64-bit range");
    }
    storage().offset_ = count;
    return *this;
}
db_query& db_query::distinct(bool enabled) {
    auto& s = storage();
    s.distinct_ = enabled;
    s.distinct_on_.clear();
    return *this;
}
db_query& db_query::distinct_on(std::span<const expr_type> expressions) {
    if (expressions.empty()) {
        throw std::invalid_argument("DISTINCT ON requires an expression");
    }
    std::pmr::vector<std::size_t> columns(resource());
    for (const auto expression : expressions) {
        columns.push_back(require_expression(expression));
    }
    auto& s = storage();
    s.distinct_on_ = std::move(columns);
    s.distinct_ = false;
    return *this;
}
db_query& db_query::returning(std::span<const expr_type> expressions) {
    std::pmr::vector<std::size_t> columns(resource());
    for (const auto expression : expressions) {
        columns.push_back(require_expression(expression));
    }
    storage().returning_ = std::move(columns);
    return *this;
}
db_query& db_query::insert_into(std::string_view table_value, std::span<const std::string_view> columns, std::string_view alias) {
    detail::require_name(table_value, true);
    if (!alias.empty()) {
        detail::require_name(alias);
    }
    auto& s = storage();
    std::pmr::vector<std::pmr::string> names(s.resource_);
    for (const auto column : columns) {
        detail::require_name(column);
        if (std::ranges::any_of(names, [&](const auto& name) { return name == column; })) {
            throw std::invalid_argument("duplicate INSERT column");
        }
        names.emplace_back(column);
    }
    s.kind_ = db_query_kind::insert;
    s.target_ = table_value;
    s.target_alias_ = alias;
    s.columns_ = std::move(names);
    return *this;
}
db_query& db_query::values(std::span<const expr_type> row) {
    auto& s = storage();
    if (s.kind_ != db_query_kind::select && s.kind_ != db_query_kind::values && s.kind_ != db_query_kind::insert) {
        throw std::invalid_argument("VALUES requires an INSERT or a values query");
    }
    std::pmr::vector<std::size_t> values(s.resource_);
    for (const auto expression : row) {
        values.push_back(require_expression(expression));
    }
    if (s.kind_ != db_query_kind::insert) {
        s.kind_ = db_query_kind::values;
    }
    s.rows_.push_back(std::move(values));
    return *this;
}
db_query& db_query::insert_from(const db_query& query) {
    if (storage().kind_ != db_query_kind::insert) {
        throw std::invalid_argument("INSERT SELECT requires an INSERT target");
    }
    query.require_select_query();
    auto snapshot = query.clone(resource());
    auto& s = storage();
    const auto index = s.queries_.size();
    s.queries_.push_back(std::move(snapshot));
    s.insert_query_ = index;
    return *this;
}
db_query& db_query::on_conflict(const db_conflict_options& conflict) {
    auto& s = storage();
    if (s.kind_ != db_query_kind::insert) {
        throw std::invalid_argument("ON CONFLICT requires an INSERT");
    }
    detail::db_stored_conflict owned(s.resource_);
    for (const auto& name : conflict.columns_) {
        detail::require_name(name);
        owned.columns_.emplace_back(name);
    }
    if (!conflict.constraint_.empty()) {
        detail::require_name(conflict.constraint_);
        owned.constraint_ = conflict.constraint_;
    }
    owned.do_nothing_ = conflict.do_nothing_;
    owned.any_unique_key_ = conflict.any_unique_key_;
    if (!conflict.target_where_.empty()) {
        owned.target_where_ = require_expression(conflict.target_where_);
    }
    if (!conflict.update_where_.empty()) {
        owned.update_where_ = require_expression(conflict.update_where_);
    }
    for (const auto& assignment : conflict.update_) {
        detail::require_name(assignment.column_);
        if (std::ranges::any_of(owned.assignments_, [&](const auto& existing) { return std::string_view(existing.column_) == assignment.column_; })) {
            throw std::invalid_argument("duplicate conflict update column");
        }
        owned.assignments_.emplace_back(assignment.column_, require_expression(assignment.value_), s.resource_);
    }
    s.conflict_ = std::move(owned);
    return *this;
}
db_query& db_query::update(std::string_view table_value, std::string_view alias) {
    detail::require_name(table_value, true);
    if (!alias.empty()) {
        detail::require_name(alias);
    }
    auto& s = storage();
    s.kind_ = db_query_kind::update;
    s.target_ = table_value;
    s.target_alias_ = alias;
    return *this;
}
db_query& db_query::set(std::string_view column, expr_type value) {
    detail::require_name(column);
    const auto expression = require_expression(value);
    auto& s = storage();
    if (s.kind_ != db_query_kind::update) {
        throw std::invalid_argument("SET requires an UPDATE");
    }
    for (auto& existing : s.assignments_) {
        if (existing.column_ == column) {
            existing.expression_ = expression;
            return *this;
        }
    }
    s.assignments_.emplace_back(column, expression, s.resource_);
    return *this;
}
db_query& db_query::update_from(std::string_view table_value, std::string_view alias) {
    if (storage().kind_ != db_query_kind::update) {
        throw std::invalid_argument("UPDATE FROM requires an UPDATE");
    }
    return from(table_value, alias);
}
db_query& db_query::update_from(const db_query& query, std::string_view alias) {
    if (storage().kind_ != db_query_kind::update) {
        throw std::invalid_argument("UPDATE FROM requires an UPDATE");
    }
    return from(query, alias);
}
db_query& db_query::delete_from(std::string_view table_value, std::string_view alias) {
    detail::require_name(table_value, true);
    if (!alias.empty()) {
        detail::require_name(alias);
    }
    auto& s = storage();
    s.kind_ = db_query_kind::delete_value;
    s.target_ = table_value;
    s.target_alias_ = alias;
    return *this;
}
db_query& db_query::delete_using(std::string_view table_value, std::string_view alias) {
    if (storage().kind_ != db_query_kind::delete_value) {
        throw std::invalid_argument("DELETE USING requires a DELETE");
    }
    return from(table_value, alias);
}
db_query& db_query::delete_using(const db_query& query, std::string_view alias) {
    if (storage().kind_ != db_query_kind::delete_value) {
        throw std::invalid_argument("DELETE USING requires a DELETE");
    }
    return from(query, alias);
}
db_query& db_query::lock(const db_lock_options& options) {
    if (options.nowait_ && options.skip_locked_) {
        throw std::invalid_argument("NOWAIT and SKIP LOCKED are mutually exclusive");
    }
    for (const auto& table : options.tables_) {
        detail::require_name(table);
    }
    storage().lock_.emplace(options, resource());
    return *this;
}
db_query& db_query::clear_lock() {
    storage().lock_.reset();
    return *this;
}
db_query& db_query::with(std::string_view name, const db_query& query, const db_cte_options& options) {
    detail::require_name(name);
    auto& s = storage();
    if (std::ranges::any_of(s.ctes_, [&](const auto& existing) { return existing.name_ == name; })) {
        throw std::invalid_argument("duplicate CTE name");
    }
    for (const auto& column : options.columns_) {
        detail::require_name(column);
    }
    auto snapshot = query.clone(resource());
    const auto index = s.queries_.size();
    s.queries_.push_back(std::move(snapshot));
    s.ctes_.emplace_back(name, index, options, s.resource_);
    return *this;
}
db_query& db_query::combine(db_set_operation operation, const db_query& query) {
    auto& s = storage();
    if ((s.kind_ != db_query_kind::select && s.kind_ != db_query_kind::values) ||
        (query.storage().kind_ != db_query_kind::select && query.storage().kind_ != db_query_kind::values)) {
        throw std::invalid_argument("set operations require SELECT or VALUES queries");
    }
    auto snapshot = query.clone(resource());
    const auto index = s.queries_.size();
    s.queries_.push_back(std::move(snapshot));
    s.set_operations_.push_back({operation, index});
    return *this;
}

}  // namespace ruvia
