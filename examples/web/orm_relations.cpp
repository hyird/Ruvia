// Prints the schema by default. --migrate --run creates and queries demo data
// in a PostgreSQL database selected by the RUVIA_DB_* environment variables.
// Build with RUVIA_ENABLE_POSTGRESQL=ON. Demonstrates relation loading,
// joins, junction tables, relation mutations and cascading entity operations.
// Keep writes in one explicit transaction when several entities must agree.
// backend_tls.h defines RUVIA_DB_TLS/CA/CERT/KEY for database transport.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/web/db/db_client.h"
#include "ruvia/web/db/db_repository.h"
#include "ruvia/web/db/db_schema.h"

#include "backend_tls.h"

namespace ruvia::examples {

struct department;
struct employee;
struct profile;
struct user;
struct course;
struct student;

RUVIA_DB_ENTITY(department, "orm_relations.orm_department",
    RUVIA_DB_COLUMN(id, std::int64_t, db_column_options{.primary_key_ = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string),
    RUVIA_DB_ONE_TO_MANY(employees, employee, department))

RUVIA_DB_ENTITY(employee, "orm_relations.orm_employee",
    RUVIA_DB_COLUMN(id, std::int64_t, db_column_options{.primary_key_ = true}),
    RUVIA_DB_COLUMN(department_id, std::int64_t),
    RUVIA_DB_COLUMN(name, std::pmr::string),
    RUVIA_DB_MANY_TO_ONE(department, department, RUVIA_DB_JOIN_COLUMN(department_id, id)))

RUVIA_DB_ENTITY(profile, "orm_relations.orm_profile",
    RUVIA_DB_COLUMN(id, std::int64_t, db_column_options{.primary_key_ = true}),
    RUVIA_DB_COLUMN(display_name, std::pmr::string),
    RUVIA_DB_ONE_TO_ONE(user, user, RUVIA_DB_INVERSE(profile)))

RUVIA_DB_ENTITY(user, "orm_relations.orm_user",
    RUVIA_DB_COLUMN(id, std::int64_t, db_column_options{.primary_key_ = true}),
    RUVIA_DB_COLUMN(profile_id, std::int64_t),
    RUVIA_DB_COLUMN(login, std::pmr::string),
    RUVIA_DB_ONE_TO_ONE(profile, profile, RUVIA_DB_JOIN_COLUMN(profile_id, id)))

using student_course_table_type = RUVIA_DB_JOIN_TABLE("orm_relations.orm_student_course",
    RUVIA_DB_JOIN_COLUMNS(RUVIA_DB_JOIN_COLUMN(student_id, id)),
    RUVIA_DB_JOIN_COLUMNS(RUVIA_DB_JOIN_COLUMN(course_id, id)));

RUVIA_DB_ENTITY(course, "orm_relations.orm_course",
    RUVIA_DB_COLUMN(id, std::int64_t, db_column_options{.primary_key_ = true}),
    RUVIA_DB_COLUMN(title, std::pmr::string),
    RUVIA_DB_MANY_TO_MANY(students, student, RUVIA_DB_INVERSE(courses)))

RUVIA_DB_ENTITY(student, "orm_relations.orm_student",
    RUVIA_DB_COLUMN(id, std::int64_t, db_column_options{.primary_key_ = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string),
    RUVIA_DB_MANY_TO_MANY(courses, course, student_course_table_type))

RUVIA_DB_ENTITY(student_course, "orm_relations.orm_student_course",
    RUVIA_DB_COLUMN(student_id, std::int64_t, db_column_options{.primary_key_ = true}),
    RUVIA_DB_COLUMN(course_id, std::int64_t, db_column_options{.primary_key_ = true}))

auto migrations() {
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_schema("orm_relations", true);
    // Referenced tables must be created before tables carrying foreign keys.
    schema.create_table<department>();
    schema.create_table<profile>();
    schema.create_table<course>();
    schema.create_table<employee>();
    schema.create_table<user>();
    schema.create_table<student>();
    schema.create_relation_tables<student>();
    return schema.compile("orm_relations_001");
}

task<void> query_example(db_client& db) {
    auto employees = db.get_repository<employee>();
    const db_find_options employee_options{
        .relations_ = {"department", "department.employees"},
        .order_ = {{"id", db_order_direction::asc}},
        .take_ = 25,
    };
    auto [rows, total] = co_await employees.find_and_count(employee_options);
    for (const auto& employee : rows) {
        const auto& department_value = employee.get<"department">();
        (void)department_value.get<"name">();
        for (const auto& colleague : department_value.get<"employees">()) {
            (void)colleague.get<"id">();
        }
    }

    auto builder = employees.create_query_builder("employee");
    builder.left_join_and_select("department", "department")
        .left_join_and_select("department.employees", "colleague")
        .take(25);
    auto [joined, joined_total] = co_await builder.get_many_and_count();
    if (rows.size() < 2 || joined.size() < 2 || total != 3 || joined_total != 3) {
        throw std::runtime_error("relation example expected at least two employees");
    }
    for (const auto& employee : joined) {
        if (employee.is_set<"department">() && !employee.is_null<"department">()) {
            (void)employee.get<"department">().get<"id">();
        }
    }
}

task<void> run(db_client& db, event_loop_attachment& attachment) {
    std::exception_ptr failure;
    try {
        co_await db.connect();
        auto departments = db.get_repository<department>();
        auto employees = db.get_repository<employee>();
        auto profiles = db.get_repository<profile>();
        auto users = db.get_repository<user>();
        auto courses = db.get_repository<course>();
        auto students = db.get_repository<student>();
        for (const auto [id, name] : {std::pair{1LL, "North"}, std::pair{2LL, "South"}}) {
            department row;
            row.set<"id">(id);
            row.set<"name">(name);
            co_await departments.upsert(row, {.conflict_paths_ = {"id"}});
        }
        for (const auto [id, department_value, name] : {std::tuple{1LL, 1LL, "Alice"}, std::tuple{2LL, 1LL, "Bob"}, std::tuple{3LL, 2LL, "Carol"}}) {
            employee row;
            row.set<"id">(id);
            row.set<"department_id">(department_value);
            row.set<"name">(name);
            co_await employees.upsert(row, {.conflict_paths_ = {"id"}});
        }
        profile profile;
        profile.set<"id">(1);
        profile.set<"display_name">("Demo user");
        co_await profiles.upsert(profile, {.conflict_paths_ = {"id"}});
        user user;
        user.set<"id">(1);
        user.set<"profile_id">(1);
        user.set<"login">("demo");
        co_await users.upsert(user, {.conflict_paths_ = {"id"}});
        for (const auto [id, title] : {std::pair{1LL, "Hydraulics"}, std::pair{2LL, "Controls"}}) {
            course course;
            course.set<"id">(id);
            course.set<"title">(title);
            co_await courses.upsert(course, {.conflict_paths_ = {"id"}});
            student student;
            student.set<"id">(id);
            student.set<"name">("Learner");
            co_await students.upsert(student, {.conflict_paths_ = {"id"}});
        }
        auto memberships = db.get_repository<student_course>();
        for (const auto [student_id, course_id] : {std::pair{1LL, 1LL}, std::pair{1LL, 2LL}, std::pair{2LL, 2LL}}) {
            student_course membership;
            membership.set<"student_id">(student_id);
            membership.set<"course_id">(course_id);
            co_await memberships.upsert(membership, {.conflict_paths_ = {"student_id", "course_id"}, .do_nothing_ = true});
        }
        co_await query_example(db);
        const db_find_options department_page_options{.relations_ = {"employees"}, .order_ = {{"id"}}, .take_ = 1};
        auto [department_page, department_count] = co_await departments.find_and_count(department_page_options);
        if (department_page.size() != 1 || department_count != 2 || department_page[0].get<"employees">().size() != 2) {
            throw std::runtime_error("one-to-many pagination truncated the employees");
        }
        auto department_value = co_await departments.find_one({.where_ = department::column<"id">() == 1, .relations_ = {"employees"}});
        if (!department_value || department_value->get<"employees">().size() != 2) {
            throw std::runtime_error("findOne did not preserve the complete collection");
        }
        auto next_page = departments.create_query_builder("d");
        next_page.left_join_and_select("d.employees", "e").order_by("id").skip(1).take(1);
        auto second = co_await next_page.get_one();
        if (!second || second->get<"id">() != 2 || second->get<"employees">().size() != 1) {
            throw std::runtime_error("relation query offset did not page departments");
        }
        next_page.set_lock({.mode_ = db_row_lock::update});
        if (co_await next_page.get_count() != 2) {
            throw std::runtime_error("relation count did not count root entities");
        }
        auto user_rows = co_await users.find({.relations_ = {"profile"}});
        auto profile_rows = co_await profiles.find({.relations_ = {"user"}});
        if (user_rows.size() != 1 || profile_rows.size() != 1 || user_rows[0].get<"profile">().get<"id">() != 1 || profile_rows[0].get<"user">().get<"id">() != 1) {
            throw std::runtime_error("one-to-one owning and inverse mapping failed");
        }
        const db_find_options course_options{.relations_ = {"courses.students"}, .order_ = {{"id"}}, .take_ = 1};
        auto [course_rows, student_count] = co_await students.find_and_count(course_options);
        if (course_rows.size() != 1 || student_count != 2 || course_rows[0].get<"courses">().size() != 2) {
            throw std::runtime_error("many-to-many pagination truncated the courses");
        }
        auto inverse_courses = co_await courses.find_one({.where_ = course::column<"id">() == 2, .relations_ = {"students"}});
        if (!inverse_courses || inverse_courses->get<"students">().size() != 2) {
            throw std::runtime_error("inverse many-to-many mapping failed");
        }
        std::cout << "Loaded relation graph: employees, one-to-one profile, and course junction.\n";
    } catch (...) {
        failure = std::current_exception();
    }
    co_await db.shutdown();
    attachment.stop();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

db_config config() {
    db_config result_value{.driver_ = db_driver::postgresql};
    result_value.tls_ = example::backend_tls("RUVIA_DB");
    auto read = [](const char* key, std::string& target) {
        if (const auto* value = std::getenv(key)) {
            target = value;
        }
    };
    read("RUVIA_DB_HOST", result_value.host_);
    read("RUVIA_DB_USER", result_value.username_);
    read("RUVIA_DB_PASSWORD", result_value.password_);
    read("RUVIA_DB_DATABASE", result_value.database_);
    if (const auto* port = std::getenv("RUVIA_DB_PORT")) {
        const auto value = std::stoul(port);
        if (value == 0 || value > 65535) {
            throw std::invalid_argument("invalid database port");
        }
        result_value.port_ = static_cast<std::uint16_t>(value);
    }
    result_value.connect_timeout_ = std::chrono::seconds(5);
    result_value.query_timeout_ = std::chrono::seconds(10);
    return result_value;
}

}  // namespace ruvia::examples

int main(int argc, char** argv) {
    try {
        bool migrate = false, execute = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--migrate") {
                migrate = true;
            } else if (std::string_view(argv[i]) == "--run") {
                execute = true;
            } else {
                throw std::invalid_argument("usage: ruvia_example_orm_relations [--migrate] [--run]");
            }
        }
        const auto changes = ruvia::examples::migrations();
        for (const auto& migration : changes) {
            std::cout << migration.sql() << ";\n";
        }
        if (migrate) {
            (void)ruvia::db_migrator::migrate(ruvia::examples::config(), changes);
        }
        if (execute) {
            asio::io_context context_value(1);
            auto attachment = ruvia::attach_event_loop(context_value);
            auto settings = ruvia::examples::config();
            ruvia::db_client db(attachment.loop(), settings);
            auto root = attachment.loop().start(ruvia::examples::run(db, attachment));
            attachment.run();
            root.get();
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
