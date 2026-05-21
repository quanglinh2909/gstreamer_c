# C++ REST API (Oat++ + PostgreSQL) — Camera CRUD Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Scaffold a C++20 REST API server using Oat++, vcpkg, oatpp-postgresql, and oatpp-swagger, with a working `Camera` CRUD example and a PostgreSQL connection pool.

**Architecture:** Layered Oat++ project (controller → service → db → dto) with DI components for shared resources (object mapper, router, connection pool, swagger). Single binary; configuration values are constants in components (config-file abstraction deferred).

**Tech Stack:** C++20, CMake ≥ 3.20, vcpkg (manifest mode), Oat++, oatpp-postgresql, oatpp-swagger, PostgreSQL ≥ 13.

---

## Reference Spec

[`docs/superpowers/specs/2026-05-19-cpp-restapi-camera-design.md`](../specs/2026-05-19-cpp-restapi-camera-design.md)

## Prerequisites

The engineer must have these installed before starting:

- `cmake ≥ 3.20`, a C++20 compiler (gcc 11+ / clang 14+).
- `vcpkg` cloned and `$VCPKG_ROOT` exported, with `$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake` available.
- PostgreSQL ≥ 13 running locally; a database `test_gstreamer` accessible via `postgresql://postgres:postgres@localhost:5432/test_gstreamer` (or update the constant in [src/DatabaseComponent.hpp](../../../src/DatabaseComponent.hpp) in Task 4).
- `git` initialized in the working directory (Task 1 handles this).

## File Structure to Be Produced

| File | Purpose |
|------|---------|
| `.gitignore` | Ignore build dirs, vcpkg installed dir, IDE files |
| `vcpkg.json` | vcpkg manifest with oatpp, oatpp-postgresql, oatpp-swagger |
| `CMakeLists.txt` | CMake build (replace existing minimal one) |
| `sql/001_init_cameras.sql` | Schema for `cameras` table |
| `src/App.cpp` | Entry point (replaces `main.cpp`) |
| `src/AppComponent.hpp` | DI: server provider, router, object mapper, error handler |
| `src/DatabaseComponent.hpp` | DI: Postgres connection pool + `CameraDb` |
| `src/SwaggerComponent.hpp` | DI: OpenAPI document info + swagger UI resources |
| `src/dto/CameraDto.hpp` | `CameraDto`, `CreateCameraDto` |
| `src/dto/StatusDto.hpp` | `StatusDto` (generic response wrapper) |
| `src/db/CameraDb.hpp` | DbClient with 5 SQL query macros |
| `src/service/CameraService.hpp` | Validation + business logic |
| `src/controller/CameraController.hpp` | HTTP endpoints `/cameras` |

The existing `main.cpp` is deleted in Task 3.

---

## Task 1: Initialize git, gitignore, and remove placeholder

**Files:**
- Create: `.gitignore`

- [ ] **Step 1: Init git repository**

Run:
```bash
cd /home/linh/CLionProjects/test_gstreamer
git init -b main
```

Expected: `Initialized empty Git repository in .../test_gstreamer/.git/`

- [ ] **Step 2: Create `.gitignore`**

Path: `.gitignore`
Content:
```
# Build
build/
cmake-build-*/
out/

# vcpkg
vcpkg_installed/

# IDE
.idea/
.vscode/
*.user

# OS
.DS_Store
Thumbs.db
```

- [ ] **Step 3: Stage and commit the design doc + plan + gitignore**

Run:
```bash
git add .gitignore docs/superpowers/specs docs/superpowers/plans
git commit -m "chore: init repo with design spec and plan"
```

Expected: 1 commit recorded.

---

## Task 2: vcpkg manifest

**Files:**
- Create: `vcpkg.json`

- [ ] **Step 1: Write `vcpkg.json`**

Path: `vcpkg.json`
Content:
```json
{
  "name": "test-gstreamer",
  "version-string": "0.1.0",
  "dependencies": [
    "oatpp",
    "oatpp-postgresql",
    "oatpp-swagger"
  ]
}
```

- [ ] **Step 2: Commit**

Run:
```bash
git add vcpkg.json
git commit -m "build: add vcpkg manifest with oatpp dependencies"
```

---

## Task 3: Replace CMakeLists.txt and remove main.cpp placeholder

**Files:**
- Modify: `CMakeLists.txt` (full replace)
- Delete: `main.cpp`

- [ ] **Step 1: Replace `CMakeLists.txt`**

Path: `CMakeLists.txt`
Content:
```cmake
cmake_minimum_required(VERSION 3.20)
project(test_gstreamer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(oatpp CONFIG REQUIRED)
find_package(oatpp-postgresql CONFIG REQUIRED)
find_package(oatpp-swagger CONFIG REQUIRED)

add_executable(test_gstreamer
    src/App.cpp
)

target_include_directories(test_gstreamer PRIVATE src)

target_link_libraries(test_gstreamer PRIVATE
    oatpp::oatpp
    oatpp::oatpp-postgresql
    oatpp::oatpp-swagger
)

# Path to swagger UI static resources, shipped with oatpp-swagger.
# Adjust if vcpkg layout differs on your system.
target_compile_definitions(test_gstreamer PRIVATE
    OATPP_SWAGGER_RES_PATH="${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/oatpp-swagger/bin/oatpp-swagger/res"
)
```

- [ ] **Step 2: Delete `main.cpp`**

Run:
```bash
git rm main.cpp
```

Expected: file removed and staged for deletion.

- [ ] **Step 3: Commit**

Run:
```bash
git add CMakeLists.txt
git commit -m "build: replace CMake config with oatpp setup, remove main.cpp"
```

---

## Task 4: DatabaseComponent.hpp (connection pool)

**Files:**
- Create: `src/DatabaseComponent.hpp`

This file defines the DI macros that build the Postgres connection pool and the `CameraDb` client. We write it before `CameraDb` exists; the next task creates `CameraDb`. To keep the project compilable after each task we'll include `CameraDb` as a forward header that we create empty here, then fill in Task 5.

- [ ] **Step 1: Create empty `src/db/CameraDb.hpp` stub** (placeholder so DatabaseComponent compiles standalone)

Path: `src/db/CameraDb.hpp`
Content:
```cpp
#ifndef test_gstreamer_CameraDb_hpp
#define test_gstreamer_CameraDb_hpp

#include "oatpp-postgresql/orm.hpp"

#include OATPP_CODEGEN_BEGIN(DbClient)

class CameraDb : public oatpp::orm::DbClient {
public:
    explicit CameraDb(const std::shared_ptr<oatpp::orm::Executor>& executor)
        : oatpp::orm::DbClient(executor) {}
};

#include OATPP_CODEGEN_END(DbClient)

#endif
```

- [ ] **Step 2: Create `src/DatabaseComponent.hpp`**

Path: `src/DatabaseComponent.hpp`
Content:
```cpp
#ifndef test_gstreamer_DatabaseComponent_hpp
#define test_gstreamer_DatabaseComponent_hpp

#include "db/CameraDb.hpp"

#include "oatpp-postgresql/ConnectionProvider.hpp"
#include "oatpp-postgresql/Executor.hpp"

#include "oatpp/core/macro/component.hpp"

class DatabaseComponent {
public:
    // Connection string is hard-coded for now (config loader is a follow-up).
    static constexpr const char* DB_URL =
        "postgresql://postgres:postgres@localhost:5432/test_gstreamer";

    OATPP_CREATE_COMPONENT(std::shared_ptr<CameraDb>, cameraDb)([] {
        auto connectionProvider =
            oatpp::postgresql::ConnectionProvider::createShared(DB_URL);

        auto connectionPool =
            oatpp::postgresql::ConnectionPool::createShared(
                connectionProvider,
                /* max connections */ 10,
                /* idle time */ std::chrono::seconds(60));

        auto executor =
            std::make_shared<oatpp::postgresql::Executor>(connectionPool);

        return std::make_shared<CameraDb>(executor);
    }());
};

#endif
```

- [ ] **Step 3: Commit**

Run:
```bash
git add src/db/CameraDb.hpp src/DatabaseComponent.hpp
git commit -m "feat(db): add DatabaseComponent with Postgres connection pool"
```

---

## Task 5: CameraDb query macros

**Files:**
- Modify: `src/db/CameraDb.hpp` (replace)

- [ ] **Step 1: Replace `src/db/CameraDb.hpp` with full version**

Path: `src/db/CameraDb.hpp`
Content:
```cpp
#ifndef test_gstreamer_CameraDb_hpp
#define test_gstreamer_CameraDb_hpp

#include "dto/CameraDto.hpp"

#include "oatpp-postgresql/orm.hpp"

#include OATPP_CODEGEN_BEGIN(DbClient)

class CameraDb : public oatpp::orm::DbClient {
public:
    explicit CameraDb(const std::shared_ptr<oatpp::orm::Executor>& executor)
        : oatpp::orm::DbClient(executor) {}

    QUERY(createCamera,
          "INSERT INTO cameras(name, rtsp, status) "
          "VALUES (:name, :rtsp, :status) "
          "RETURNING *;",
          PARAM(oatpp::String, name),
          PARAM(oatpp::String, rtsp),
          PARAM(oatpp::String, status))

    QUERY(getCameraById,
          "SELECT * FROM cameras WHERE id = :id LIMIT 1;",
          PARAM(oatpp::Int64, id))

    QUERY(getAllCameras,
          "SELECT * FROM cameras ORDER BY id LIMIT :limit OFFSET :offset;",
          PARAM(oatpp::UInt64, limit),
          PARAM(oatpp::UInt64, offset))

    QUERY(updateCamera,
          "UPDATE cameras SET name = :name, rtsp = :rtsp, status = :status "
          "WHERE id = :id RETURNING *;",
          PARAM(oatpp::Int64, id),
          PARAM(oatpp::String, name),
          PARAM(oatpp::String, rtsp),
          PARAM(oatpp::String, status))

    QUERY(deleteCamera,
          "DELETE FROM cameras WHERE id = :id;",
          PARAM(oatpp::Int64, id))
};

#include OATPP_CODEGEN_END(DbClient)

#endif
```

Note: this file includes `dto/CameraDto.hpp` so query results can be fetched into DTOs. The DTO is created in Task 6 — order matters; do Task 6 immediately after this.

- [ ] **Step 2: Commit**

Run:
```bash
git add src/db/CameraDb.hpp
git commit -m "feat(db): add CRUD query macros for cameras"
```

---

## Task 6: CameraDto and StatusDto

**Files:**
- Create: `src/dto/CameraDto.hpp`
- Create: `src/dto/StatusDto.hpp`

- [ ] **Step 1: Create `src/dto/CameraDto.hpp`**

Path: `src/dto/CameraDto.hpp`
Content:
```cpp
#ifndef test_gstreamer_CameraDto_hpp
#define test_gstreamer_CameraDto_hpp

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

class CameraDto : public oatpp::DTO {
    DTO_INIT(CameraDto, DTO)

    DTO_FIELD_INFO(id) { info->description = "Camera id"; }
    DTO_FIELD(Int64, id);

    DTO_FIELD_INFO(name) { info->description = "Display name"; }
    DTO_FIELD(String, name);

    DTO_FIELD_INFO(rtsp) { info->description = "RTSP stream URL"; }
    DTO_FIELD(String, rtsp);

    DTO_FIELD_INFO(status) { info->description = "online | offline | error"; }
    DTO_FIELD(String, status);
};

class CreateCameraDto : public oatpp::DTO {
    DTO_INIT(CreateCameraDto, DTO)

    DTO_FIELD(String, name);
    DTO_FIELD(String, rtsp);
    DTO_FIELD(String, status);  // optional; defaults to "offline" in service
};

#include OATPP_CODEGEN_END(DTO)

#endif
```

- [ ] **Step 2: Create `src/dto/StatusDto.hpp`**

Path: `src/dto/StatusDto.hpp`
Content:
```cpp
#ifndef test_gstreamer_StatusDto_hpp
#define test_gstreamer_StatusDto_hpp

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

class StatusDto : public oatpp::DTO {
    DTO_INIT(StatusDto, DTO)

    DTO_FIELD(Int32,  statusCode);
    DTO_FIELD(String, message);
};

#include OATPP_CODEGEN_END(DTO)

#endif
```

- [ ] **Step 3: Commit**

Run:
```bash
git add src/dto/CameraDto.hpp src/dto/StatusDto.hpp
git commit -m "feat(dto): add CameraDto, CreateCameraDto, StatusDto"
```

---

## Task 7: CameraService (validation + business logic)

**Files:**
- Create: `src/service/CameraService.hpp`

- [ ] **Step 1: Create `src/service/CameraService.hpp`**

Path: `src/service/CameraService.hpp`
Content:
```cpp
#ifndef test_gstreamer_CameraService_hpp
#define test_gstreamer_CameraService_hpp

#include "db/CameraDb.hpp"
#include "dto/CameraDto.hpp"
#include "dto/StatusDto.hpp"

#include "oatpp/core/macro/component.hpp"
#include "oatpp/web/protocol/http/Http.hpp"

class CameraService {
public:
    using Status = oatpp::web::protocol::http::Status;

    oatpp::Object<CameraDto> createCamera(const oatpp::Object<CreateCameraDto>& in) {
        validate(in);
        auto status = in->status ? in->status : oatpp::String("offline");

        auto res = m_db->createCamera(in->name, in->rtsp, status);
        assertSuccess(res);
        return fetchOne(res, Status::CODE_500, "Failed to create camera");
    }

    oatpp::Object<CameraDto> getCameraById(const oatpp::Int64& id) {
        auto res = m_db->getCameraById(id);
        assertSuccess(res);
        return fetchOne(res, Status::CODE_404, "Camera not found");
    }

    oatpp::List<oatpp::Object<CameraDto>>
    getAllCameras(const oatpp::UInt64& limit, const oatpp::UInt64& offset) {
        auto res = m_db->getAllCameras(limit, offset);
        assertSuccess(res);
        return res->fetch<oatpp::List<oatpp::Object<CameraDto>>>();
    }

    oatpp::Object<CameraDto> putCamera(
        const oatpp::Int64& id,
        const oatpp::Object<CreateCameraDto>& in)
    {
        validate(in);
        auto status = in->status ? in->status : oatpp::String("offline");

        auto res = m_db->updateCamera(id, in->name, in->rtsp, status);
        assertSuccess(res);
        return fetchOne(res, Status::CODE_404, "Camera not found");
    }

    oatpp::Object<StatusDto> deleteCamera(const oatpp::Int64& id) {
        auto res = m_db->deleteCamera(id);
        assertSuccess(res);
        auto dto = StatusDto::createShared();
        dto->statusCode = 200;
        dto->message = "Deleted";
        return dto;
    }

private:
    OATPP_COMPONENT(std::shared_ptr<CameraDb>, m_db);

    static void validate(const oatpp::Object<CreateCameraDto>& in) {
        OATPP_ASSERT_HTTP(in,         Status::CODE_400, "Body required");
        OATPP_ASSERT_HTTP(in->name && in->name->size() > 0,
                          Status::CODE_400, "name required");
        OATPP_ASSERT_HTTP(in->rtsp && in->rtsp->size() > 0,
                          Status::CODE_400, "rtsp required");
        OATPP_ASSERT_HTTP(in->rtsp->find("rtsp://") == 0,
                          Status::CODE_400, "rtsp must start with rtsp://");
        if (in->status) {
            auto s = in->status;
            OATPP_ASSERT_HTTP(s == "online" || s == "offline" || s == "error",
                              Status::CODE_400, "status must be online|offline|error");
        }
    }

    template <class Res>
    static void assertSuccess(const std::shared_ptr<Res>& res) {
        OATPP_ASSERT_HTTP(res->isSuccess(),
                          Status::CODE_500,
                          res->getErrorMessage()->c_str());
    }

    template <class Res>
    static oatpp::Object<CameraDto> fetchOne(
        const std::shared_ptr<Res>& res,
        const oatpp::web::protocol::http::Status& notFoundStatus,
        const char* notFoundMsg)
    {
        OATPP_ASSERT_HTTP(res->hasMoreToFetch(), notFoundStatus, notFoundMsg);
        auto list = res->template fetch<oatpp::List<oatpp::Object<CameraDto>>>();
        OATPP_ASSERT_HTTP(list && list->size() > 0, notFoundStatus, notFoundMsg);
        return list[0];
    }
};

#endif
```

- [ ] **Step 2: Commit**

Run:
```bash
git add src/service/CameraService.hpp
git commit -m "feat(service): add CameraService with validation and CRUD"
```

---

## Task 8: CameraController (HTTP endpoints)

**Files:**
- Create: `src/controller/CameraController.hpp`

- [ ] **Step 1: Create `src/controller/CameraController.hpp`**

Path: `src/controller/CameraController.hpp`
Content:
```cpp
#ifndef test_gstreamer_CameraController_hpp
#define test_gstreamer_CameraController_hpp

#include "service/CameraService.hpp"

#include "oatpp/web/server/api/ApiController.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/core/macro/component.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

class CameraController : public oatpp::web::server::api::ApiController {
public:
    explicit CameraController(
        OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

    ENDPOINT_INFO(getAll) {
        info->summary = "List cameras";
        info->addResponse<oatpp::List<oatpp::Object<CameraDto>>>(
            Status::CODE_200, "application/json");
    }
    ENDPOINT("GET", "/cameras", getAll,
             QUERY(oatpp::UInt64, limit,  "limit",  "50"),
             QUERY(oatpp::UInt64, offset, "offset", "0"))
    {
        return createDtoResponse(Status::CODE_200,
                                 m_service.getAllCameras(limit, offset));
    }

    ENDPOINT_INFO(getOne) {
        info->summary = "Get a camera by id";
        info->addResponse<oatpp::Object<CameraDto>>(
            Status::CODE_200, "application/json");
        info->addResponse<oatpp::Object<StatusDto>>(
            Status::CODE_404, "application/json");
    }
    ENDPOINT("GET", "/cameras/{id}", getOne,
             PATH(oatpp::Int64, id))
    {
        return createDtoResponse(Status::CODE_200,
                                 m_service.getCameraById(id));
    }

    ENDPOINT_INFO(create) {
        info->summary = "Create a camera";
        info->addConsumes<oatpp::Object<CreateCameraDto>>("application/json");
        info->addResponse<oatpp::Object<CameraDto>>(
            Status::CODE_201, "application/json");
        info->addResponse<oatpp::Object<StatusDto>>(
            Status::CODE_400, "application/json");
    }
    ENDPOINT("POST", "/cameras", create,
             BODY_DTO(oatpp::Object<CreateCameraDto>, dto))
    {
        return createDtoResponse(Status::CODE_201,
                                 m_service.createCamera(dto));
    }

    ENDPOINT_INFO(update) {
        info->summary = "Update a camera";
        info->addConsumes<oatpp::Object<CreateCameraDto>>("application/json");
        info->addResponse<oatpp::Object<CameraDto>>(
            Status::CODE_200, "application/json");
        info->addResponse<oatpp::Object<StatusDto>>(
            Status::CODE_404, "application/json");
    }
    ENDPOINT("PUT", "/cameras/{id}", update,
             PATH(oatpp::Int64, id),
             BODY_DTO(oatpp::Object<CreateCameraDto>, dto))
    {
        return createDtoResponse(Status::CODE_200,
                                 m_service.putCamera(id, dto));
    }

    ENDPOINT_INFO(remove) {
        info->summary = "Delete a camera";
        info->addResponse<oatpp::Object<StatusDto>>(
            Status::CODE_200, "application/json");
        info->addResponse<oatpp::Object<StatusDto>>(
            Status::CODE_404, "application/json");
    }
    ENDPOINT("DELETE", "/cameras/{id}", remove,
             PATH(oatpp::Int64, id))
    {
        return createDtoResponse(Status::CODE_200,
                                 m_service.deleteCamera(id));
    }

private:
    CameraService m_service;
};

#include OATPP_CODEGEN_END(ApiController)

#endif
```

- [ ] **Step 2: Commit**

Run:
```bash
git add src/controller/CameraController.hpp
git commit -m "feat(controller): add CameraController with 5 REST endpoints"
```

---

## Task 9: AppComponent (router, server provider, object mapper)

**Files:**
- Create: `src/AppComponent.hpp`

- [ ] **Step 1: Create `src/AppComponent.hpp`**

Path: `src/AppComponent.hpp`
Content:
```cpp
#ifndef test_gstreamer_AppComponent_hpp
#define test_gstreamer_AppComponent_hpp

#include "oatpp/web/server/HttpConnectionHandler.hpp"
#include "oatpp/network/tcp/server/ConnectionProvider.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/web/server/HttpRouter.hpp"
#include "oatpp/core/macro/component.hpp"

class AppComponent {
public:
    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::network::ServerConnectionProvider>, serverConnectionProvider)([] {
        return oatpp::network::tcp::server::ConnectionProvider::createShared(
            { "0.0.0.0", 8000, oatpp::network::Address::IP_4 });
    }());

    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::web::server::HttpRouter>, httpRouter)([] {
        return oatpp::web::server::HttpRouter::createShared();
    }());

    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::network::ConnectionHandler>, serverConnectionHandler)([] {
        OATPP_COMPONENT(std::shared_ptr<oatpp::web::server::HttpRouter>, router);
        return oatpp::web::server::HttpConnectionHandler::createShared(router);
    }());

    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::data::mapping::ObjectMapper>, apiObjectMapper)([] {
        auto mapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
        mapper->getSerializer()->getConfig()->useBeautifier = true;
        return mapper;
    }());
};

#endif
```

- [ ] **Step 2: Commit**

Run:
```bash
git add src/AppComponent.hpp
git commit -m "feat(app): add AppComponent for router, server provider, object mapper"
```

---

## Task 10: SwaggerComponent

**Files:**
- Create: `src/SwaggerComponent.hpp`

- [ ] **Step 1: Create `src/SwaggerComponent.hpp`**

Path: `src/SwaggerComponent.hpp`
Content:
```cpp
#ifndef test_gstreamer_SwaggerComponent_hpp
#define test_gstreamer_SwaggerComponent_hpp

#include "oatpp-swagger/Model.hpp"
#include "oatpp-swagger/Resources.hpp"
#include "oatpp/core/macro/component.hpp"

class SwaggerComponent {
public:
    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::swagger::DocumentInfo>, swaggerDocumentInfo)([] {
        oatpp::swagger::DocumentInfo::Builder builder;
        builder
            .setTitle("test_gstreamer API")
            .setDescription("REST API for Camera management")
            .setVersion("1.0")
            .setContactName("dev")
            .setContactUrl("https://example.local/")
            .addServer("http://localhost:8000", "Local dev");
        return builder.build();
    }());

    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::swagger::Resources>, swaggerResources)([] {
        return oatpp::swagger::Resources::loadResources(OATPP_SWAGGER_RES_PATH);
    }());
};

#endif
```

- [ ] **Step 2: Commit**

Run:
```bash
git add src/SwaggerComponent.hpp
git commit -m "feat(swagger): add SwaggerComponent with API metadata"
```

---

## Task 11: App.cpp entry point

**Files:**
- Create: `src/App.cpp`

- [ ] **Step 1: Create `src/App.cpp`**

Path: `src/App.cpp`
Content:
```cpp
#include "AppComponent.hpp"
#include "DatabaseComponent.hpp"
#include "SwaggerComponent.hpp"
#include "controller/CameraController.hpp"

#include "oatpp-swagger/Controller.hpp"
#include "oatpp/network/Server.hpp"

#include <csignal>
#include <iostream>

namespace {
std::shared_ptr<oatpp::network::Server> g_server;

void onSignal(int) {
    if (g_server) g_server->stop();
}
}

void run() {
    AppComponent      appComponents;
    SwaggerComponent  swaggerComponents;
    DatabaseComponent databaseComponents;

    OATPP_COMPONENT(std::shared_ptr<oatpp::web::server::HttpRouter>, router);

    auto cameraController = std::make_shared<CameraController>();
    cameraController->addEndpointsToRouter(router);

    auto swaggerController = oatpp::swagger::Controller::createShared(
        cameraController->getEndpoints());
    swaggerController->addEndpointsToRouter(router);

    OATPP_COMPONENT(std::shared_ptr<oatpp::network::ServerConnectionProvider>, provider);
    OATPP_COMPONENT(std::shared_ptr<oatpp::network::ConnectionHandler>,        handler);

    g_server = oatpp::network::Server::createShared(provider, handler);

    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    std::cout << "Server running on http://"
              << provider->getProperty("host").toString()->c_str()
              << ":"
              << provider->getProperty("port").toString()->c_str()
              << "  (Swagger UI: /swagger/ui)" << std::endl;

    g_server->run();
}

int main() {
    oatpp::base::Environment::init();
    run();
    oatpp::base::Environment::destroy();
    return 0;
}
```

- [ ] **Step 2: Commit**

Run:
```bash
git add src/App.cpp
git commit -m "feat(app): add App.cpp entry point wiring controllers and swagger"
```

---

## Task 12: SQL schema

**Files:**
- Create: `sql/001_init_cameras.sql`

- [ ] **Step 1: Create `sql/001_init_cameras.sql`**

Path: `sql/001_init_cameras.sql`
Content:
```sql
CREATE TABLE IF NOT EXISTS cameras (
  id       BIGSERIAL PRIMARY KEY,
  name     VARCHAR(128) NOT NULL,
  rtsp     VARCHAR(512) NOT NULL,
  status   VARCHAR(16)  NOT NULL DEFAULT 'offline'
);
CREATE INDEX IF NOT EXISTS idx_cameras_status ON cameras(status);
```

- [ ] **Step 2: Commit**

Run:
```bash
git add sql/001_init_cameras.sql
git commit -m "feat(db): add initial cameras schema"
```

---

## Task 13: Configure CMake with vcpkg toolchain and build

**Files:** none — build verification only.

- [ ] **Step 1: Verify `VCPKG_ROOT` is set**

Run:
```bash
echo "$VCPKG_ROOT"
test -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" && echo OK
```

Expected: prints a non-empty path and `OK`. If empty, install vcpkg and `export VCPKG_ROOT=...` before continuing.

- [ ] **Step 2: Configure**

Run from the project root:
```bash
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_BUILD_TYPE=Debug
```

Expected: vcpkg installs `oatpp`, `oatpp-postgresql`, `oatpp-swagger` on first run (may take several minutes); configure prints `-- Configuring done` and `-- Generating done`.

- [ ] **Step 3: Build**

Run:
```bash
cmake --build build -j
```

Expected: ends with `[100%] Built target test_gstreamer`. If compile errors appear, address them — common causes:
- Missing include — add to the affected header.
- Wrong `OATPP_SWAGGER_RES_PATH` — open the swagger package's `share/` directory under `vcpkg_installed/<triplet>/share/oatpp-swagger/` and adjust the path in `CMakeLists.txt`.
- Type mismatch between DTO field and SQL column — verify SQL columns match DTO field types in `CameraDto`.

- [ ] **Step 4: Commit any fixes**

If you had to amend `CMakeLists.txt` or any source file:
```bash
git add -u
git commit -m "fix(build): adjust paths / includes after first compile"
```

---

## Task 14: Run the server and smoke-test endpoints

**Files:** none — runtime verification only.

- [ ] **Step 1: Prepare database**

Run:
```bash
psql -U postgres -c "CREATE DATABASE test_gstreamer;" || true
psql -U postgres -d test_gstreamer -f sql/001_init_cameras.sql
```

Expected: schema applied; rerunning is safe (uses `IF NOT EXISTS`).

If your local Postgres user / password isn't `postgres:postgres`, update `DB_URL` in `src/DatabaseComponent.hpp`, rebuild, then re-run.

- [ ] **Step 2: Start the server**

Run:
```bash
./build/test_gstreamer
```

Expected stdout: `Server running on http://0.0.0.0:8000  (Swagger UI: /swagger/ui)`. Leave it running in this terminal.

- [ ] **Step 3: Open Swagger UI**

Open `http://localhost:8000/swagger/ui` in a browser.
Expected: Swagger page shows 5 endpoints under `default` tag (`GET /cameras`, `GET /cameras/{id}`, `POST /cameras`, `PUT /cameras/{id}`, `DELETE /cameras/{id}`).

- [ ] **Step 4: Smoke test endpoints**

In a second terminal:
```bash
curl -s -X POST http://localhost:8000/cameras \
  -H 'Content-Type: application/json' \
  -d '{"name":"Cam 1","rtsp":"rtsp://192.168.1.10/stream","status":"online"}'
```
Expected: 201 response with JSON body `{"id":1,"name":"Cam 1","rtsp":"rtsp://...","status":"online"}`.

```bash
curl -s http://localhost:8000/cameras
```
Expected: JSON array with one element.

```bash
curl -s http://localhost:8000/cameras/1
```
Expected: JSON object with `id:1`.

```bash
curl -s -X PUT http://localhost:8000/cameras/1 \
  -H 'Content-Type: application/json' \
  -d '{"name":"Cam 1","rtsp":"rtsp://192.168.1.10/stream","status":"offline"}'
```
Expected: same object with `status:"offline"`.

```bash
curl -s -X DELETE http://localhost:8000/cameras/1
```
Expected: `{"statusCode":200,"message":"Deleted"}`.

- [ ] **Step 5: Validation errors**

```bash
curl -s -i -X POST http://localhost:8000/cameras \
  -H 'Content-Type: application/json' \
  -d '{"name":"Bad","rtsp":"http://wrong","status":"online"}'
```
Expected: HTTP 400 with message `rtsp must start with rtsp://`.

```bash
curl -s -i http://localhost:8000/cameras/999999
```
Expected: HTTP 404 with message `Camera not found`.

- [ ] **Step 6: Stop the server**

In the server terminal press `Ctrl+C`. Expected: server shuts down cleanly.

- [ ] **Step 7: Commit nothing — verification only**

No commit; this task is a runtime gate.

---

## Self-Review

### Spec coverage

| Spec section | Implemented in task |
|---|---|
| §2 Architecture | Tasks 4, 9, 10, 11 |
| §3 Folder structure | All file-creating tasks |
| §4 Components | 4 (DatabaseComponent), 5 (CameraDb), 6 (DTOs), 7 (Service), 8 (Controller), 9 (AppComponent), 10 (SwaggerComponent), 11 (App.cpp) |
| §5 Endpoints / data flow | Tasks 8 (controller wiring) + 7 (service flow); smoke-tested in 14 |
| §6 Error handling | Task 7 (`OATPP_ASSERT_HTTP`); verified in Task 14 Step 5 |
| §7 SQL schema | Task 12 |
| §8 Build / run / smoke test | Tasks 13 + 14 |
| §9 Testing scaffold | Deferred — spec marks scaffold as out of scope |
| §10 Non-goals | None implemented, as required |

All in-scope spec items are covered.

### Placeholder scan

No `TBD`, `TODO`, "implement later", "similar to Task N", or "add appropriate error handling". Every step contains either explicit content or a runnable command.

### Type consistency

- `CameraDto`/`CreateCameraDto` field names (`id`, `name`, `rtsp`, `status`) match SQL columns and query macro `PARAM` names.
- Service method names (`createCamera`, `getCameraById`, `getAllCameras`, `putCamera`, `deleteCamera`) match controller calls in Task 8.
- `m_db` (in Service) and `cameraDb` (in DatabaseComponent) use the standard `OATPP_COMPONENT(std::shared_ptr<CameraDb>, ...)` pattern.
- `Status::CODE_404` / `CODE_400` / `CODE_500` are consistent between service throws and spec error table.

Plan complete.
