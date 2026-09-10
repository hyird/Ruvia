// Prints the schema by default. --migrate --run creates and queries demo data
// in a PostgreSQL database selected by the RUVIA_DB_* environment variables.

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

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/web/db/DbClient.h"
#include "ruvia/web/db/DbRepository.h"
#include "ruvia/web/db/DbSchema.h"

namespace ruvia::examples {

struct Department;
struct Employee;
struct Profile;
struct User;
struct Course;
struct Student;

RUVIA_DB_ENTITY(Department, "orm_relations.orm_department",
    RUVIA_DB_COLUMN(id, std::int64_t, DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string),
    RUVIA_DB_ONE_TO_MANY(employees, Employee, department))

RUVIA_DB_ENTITY(Employee, "orm_relations.orm_employee",
    RUVIA_DB_COLUMN(id, std::int64_t, DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(department_id, std::int64_t),
    RUVIA_DB_COLUMN(name, std::pmr::string),
    RUVIA_DB_MANY_TO_ONE(department, Department, RUVIA_DB_JOIN_COLUMN(department_id, id)))

RUVIA_DB_ENTITY(Profile, "orm_relations.orm_profile",
    RUVIA_DB_COLUMN(id, std::int64_t, DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(display_name, std::pmr::string),
    RUVIA_DB_ONE_TO_ONE(user, User, RUVIA_DB_INVERSE(profile)))

RUVIA_DB_ENTITY(User, "orm_relations.orm_user",
    RUVIA_DB_COLUMN(id, std::int64_t, DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(profile_id, std::int64_t),
    RUVIA_DB_COLUMN(login, std::pmr::string),
    RUVIA_DB_ONE_TO_ONE(profile, Profile, RUVIA_DB_JOIN_COLUMN(profile_id, id)))

using StudentCourseTable = RUVIA_DB_JOIN_TABLE("orm_relations.orm_student_course",
    RUVIA_DB_JOIN_COLUMNS(RUVIA_DB_JOIN_COLUMN(student_id, id)),
    RUVIA_DB_JOIN_COLUMNS(RUVIA_DB_JOIN_COLUMN(course_id, id)));

RUVIA_DB_ENTITY(Course, "orm_relations.orm_course",
    RUVIA_DB_COLUMN(id, std::int64_t, DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(title, std::pmr::string),
    RUVIA_DB_MANY_TO_MANY(students, Student, RUVIA_DB_INVERSE(courses)))

RUVIA_DB_ENTITY(Student, "orm_relations.orm_student",
    RUVIA_DB_COLUMN(id, std::int64_t, DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string),
    RUVIA_DB_MANY_TO_MANY(courses, Course, StudentCourseTable))

auto migrations() {
    DbSchema schema({.driver = DbDriver::kPostgreSql});
    schema.createSchema("orm_relations", true);
    // Referenced tables must be created before tables carrying foreign keys.
    schema.createTable<Department>();
    schema.createTable<Profile>();
    schema.createTable<Course>();
    schema.createTable<Employee>();
    schema.createTable<User>();
    schema.createTable<Student>();
    schema.createRelationTables<Student>();
    return schema.compile("orm_relations_001");
}

Task<void> queryExample(DbClient& db) {
    auto employees = db.getRepository<Employee>();
    const DbFindOptions employeeOptions{
        .relations = {"department", "department.employees"},
        .order = {{"id", DbOrderDirection::kAsc}},
        .take = 25,
    };
    auto rows = co_await employees.find(employeeOptions);
    for (const auto& employee : rows) {
        const auto& department = employee.get<"department">();
        (void)department.get<"name">();
        for (const auto& colleague : department.get<"employees">()) {
            (void)colleague.get<"id">();
        }
    }

    auto builder = employees.createQueryBuilder("employee");
    builder.leftJoinAndSelect("department", "department")
        .leftJoinAndSelect("department.employees", "colleague")
        .take(25);
    auto joined = co_await builder.getMany();
    if (rows.size() < 2 || joined.size() < 2) {
        throw std::runtime_error("relation example expected at least two employees");
    }
    for (const auto& employee : joined) {
        if (employee.isSet<"department">() && !employee.isNull<"department">()) {
            (void)employee.get<"department">().get<"id">();
        }
    }
}

Task<void> run(DbClient& db, EventLoopAttachment& attachment) {
    std::exception_ptr failure;
    try {
        co_await db.connect();
        auto departments = db.getRepository<Department>();
        auto employees = db.getRepository<Employee>();
        auto profiles = db.getRepository<Profile>();
        auto users = db.getRepository<User>();
        auto courses = db.getRepository<Course>();
        auto students = db.getRepository<Student>();
        for (const auto [id, name] : {std::pair{1LL, "North"}, std::pair{2LL, "South"}}) {
            Department row;
            row.set<"id">(id);
            row.set<"name">(name);
            co_await departments.upsert(row, {.conflictPaths = {"id"}});
        }
        for (const auto [id, department, name] : {std::tuple{1LL, 1LL, "Alice"}, std::tuple{2LL, 1LL, "Bob"}, std::tuple{3LL, 2LL, "Carol"}}) {
            Employee row;
            row.set<"id">(id);
            row.set<"department_id">(department);
            row.set<"name">(name);
            co_await employees.upsert(row, {.conflictPaths = {"id"}});
        }
        Profile profile;
        profile.set<"id">(1);
        profile.set<"display_name">("Demo user");
        co_await profiles.upsert(profile, {.conflictPaths = {"id"}});
        User user;
        user.set<"id">(1);
        user.set<"profile_id">(1);
        user.set<"login">("demo");
        co_await users.upsert(user, {.conflictPaths = {"id"}});
        for (const auto [id, title] : {std::pair{1LL, "Hydraulics"}, std::pair{2LL, "Controls"}}) {
            Course course;
            course.set<"id">(id);
            course.set<"title">(title);
            co_await courses.upsert(course, {.conflictPaths = {"id"}});
            Student student;
            student.set<"id">(id);
            student.set<"name">("Learner");
            co_await students.upsert(student, {.conflictPaths = {"id"}});
        }
        DbQuery junction;
        junction.insertInto(StudentCourseTable::name.view(), {"student_id", "course_id"})
            .values({junction.value(1), junction.value(1)})
            .values({junction.value(1), junction.value(2)})
            .values({junction.value(2), junction.value(2)})
            .onConflict({.columns = {"student_id", "course_id"}, .doNothing = true});
        co_await db.execute(junction);
        co_await queryExample(db);
        const DbFindOptions departmentPageOptions{.relations = {"employees"}, .order = {{"id"}}, .take = 1};
        auto departmentPage = co_await departments.find(departmentPageOptions);
        if (departmentPage.size() != 1 || departmentPage[0].get<"employees">().size() != 2) {
            throw std::runtime_error("one-to-many pagination truncated the employees");
        }
        auto department = co_await departments.findOne({.where = Department::column<"id">() == 1, .relations = {"employees"}});
        if (!department || department->get<"employees">().size() != 2) {
            throw std::runtime_error("findOne did not preserve the complete collection");
        }
        auto nextPage = departments.createQueryBuilder("d");
        nextPage.leftJoinAndSelect("d.employees", "e").orderBy(nextPage.column<"id">()).skip(1).take(1);
        auto second = co_await nextPage.getOne();
        if (!second || second->get<"id">() != 2 || second->get<"employees">().size() != 1) {
            throw std::runtime_error("relation query offset did not page departments");
        }
        nextPage.setLock({.mode = DbRowLock::kUpdate});
        if (co_await nextPage.getCount() != 2) {
            throw std::runtime_error("relation count did not count root entities");
        }
        auto userRows = co_await users.find({.relations = {"profile"}});
        auto profileRows = co_await profiles.find({.relations = {"user"}});
        if (userRows.size() != 1 || profileRows.size() != 1 || userRows[0].get<"profile">().get<"id">() != 1 || profileRows[0].get<"user">().get<"id">() != 1) {
            throw std::runtime_error("one-to-one owning and inverse mapping failed");
        }
        const DbFindOptions courseOptions{.relations = {"courses.students"}, .order = {{"id"}}, .take = 1};
        auto courseRows = co_await students.find(courseOptions);
        if (courseRows.size() != 1 || courseRows[0].get<"courses">().size() != 2) {
            throw std::runtime_error("many-to-many pagination truncated the courses");
        }
        auto inverseCourses = co_await courses.findOne({.where = Course::column<"id">() == 2, .relations = {"students"}});
        if (!inverseCourses || inverseCourses->get<"students">().size() != 2) {
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

DbConfig config() {
    DbConfig result{.driver = DbDriver::kPostgreSql};
    auto read = [](const char* key, std::string& target) {
        if (const auto* value = std::getenv(key)) {
            target = value;
        }
    };
    read("RUVIA_DB_HOST", result.host);
    read("RUVIA_DB_USER", result.username);
    read("RUVIA_DB_PASSWORD", result.password);
    read("RUVIA_DB_DATABASE", result.database);
    if (const auto* port = std::getenv("RUVIA_DB_PORT")) {
        const auto value = std::stoul(port);
        if (value == 0 || value > 65535) {
            throw std::invalid_argument("invalid database port");
        }
        result.port = static_cast<std::uint16_t>(value);
    }
    result.connectTimeout = std::chrono::seconds(5);
    result.queryTimeout = std::chrono::seconds(10);
    return result;
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
            (void)ruvia::DbMigrator::migrate(ruvia::examples::config(), changes);
        }
        if (execute) {
            asio::io_context context(1);
            auto attachment = ruvia::attachEventLoop(context);
            auto settings = ruvia::examples::config();
            ruvia::DbClient db(attachment.loop(), settings);
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
