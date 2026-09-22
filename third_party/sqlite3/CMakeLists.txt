# third_party/sqlite3 — vendored SQLite 3 amalgamation (TP-1).
# Law: qiven-devkit docs/engineering/third-party-dependencies.md.
# Compile-flag adaptation is scoped to THIS target only; nothing here
# may leak into first-party targets (namespaced qiven::tp::sqlite3).
add_library(qiven-tp-sqlite3 STATIC sqlite3.c)
set_target_properties(qiven-tp-sqlite3 PROPERTIES LINKER_LANGUAGE C)
add_library(qiven::tp::sqlite3 ALIAS qiven-tp-sqlite3)

set_target_properties(
    qiven-tp-sqlite3
    PROPERTIES
        CXX_EXTENSIONS OFF
        FOLDER "ThirdParty"
        DISABLE_PRECOMPILE_HEADERS ON
)

target_include_directories(qiven-tp-sqlite3 PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

# Upstream amalgamation: C, relaxed warnings, CRT deprecation silenced —
# all PRIVATE, on this target only.
if(MSVC)
    target_compile_options(qiven-tp-sqlite3 PRIVATE /W3 /utf-8)
else()
    target_compile_options(qiven-tp-sqlite3 PRIVATE -W3)
endif()
target_compile_definitions(
    qiven-tp-sqlite3
    PRIVATE
        _CRT_SECURE_NO_WARNINGS
        SQLITE_THREADSAFE=1 # serialized mode; single-writer discipline keeps it cheap
        SQLITE_OMIT_LOAD_EXTENSION # no dynamic extension loading from a governed host
        SQLITE_OMIT_DEPRECATED
        SQLITE_DEFAULT_FOREIGN_KEYS=0 # runtime enables per-connection (design §7.1)
        SQLITE_DEFAULT_WAL_SYNCHRONOUS=1 # FULL (design §7.1 configuration law)
        SQLITE_USE_ALLOCA=0
)

target_link_libraries(qiven-tp-sqlite3 PUBLIC advapi32)
