// Copyright 2023 Huawei Cloud Computing Technology Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/buildtool/execution_api/execution_service/execution_server.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>

#include "fmt/format.h"
#include "gsl-lite/gsl-lite.hpp"
#include "src/buildtool/compatibility/native_support.hpp"
#include "src/buildtool/execution_api/local/garbage_collector.hpp"
#include "src/buildtool/file_system/file_system_manager.hpp"

auto ExecutionServiceImpl::GetAction(::bazel_re::ExecuteRequest const* request)
    const noexcept -> std::pair<std::optional<::bazel_re::Action>,
                                std::optional<std::string>> {
    // get action description
    auto path = storage_.BlobPath(request->action_digest(), false);
    if (!path) {
        auto str = fmt::format(
            "could not retrieve blob {} from cas",
            NativeSupport::Unprefix(request->action_digest().hash()));
        logger_.Emit(LogLevel::Error, str);
        return {std::nullopt, str};
    }
    ::bazel_re::Action action{};
    {
        std::ifstream f(*path);
        if (!action.ParseFromIstream(&f)) {
            auto str = fmt::format(
                "failed to parse action from blob {}",
                NativeSupport::Unprefix(request->action_digest().hash()));
            logger_.Emit(LogLevel::Error, str);
            return {std::nullopt, str};
        }
    }

    path = Compatibility::IsCompatible()
               ? storage_.BlobPath(action.input_root_digest(), false)
               : storage_.TreePath(action.input_root_digest());

    if (!path) {
        auto str = fmt::format(
            "could not retrieve input root {} from cas",
            NativeSupport::Unprefix(action.input_root_digest().hash()));
        logger_.Emit(LogLevel::Error, str);
        return {std::nullopt, str};
    }
    return {std::move(action), std::nullopt};
}

auto ExecutionServiceImpl::GetCommand(::bazel_re::Action const& action)
    const noexcept -> std::pair<std::optional<::bazel_re::Command>,
                                std::optional<std::string>> {

    auto path = storage_.BlobPath(action.command_digest(), false);
    if (!path) {
        auto str = fmt::format(
            "could not retrieve blob {} from cas",
            NativeSupport::Unprefix(action.command_digest().hash()));
        logger_.Emit(LogLevel::Error, str);
        return {std::nullopt, str};
    }

    ::bazel_re::Command c{};
    {
        std::ifstream f(*path);
        if (!c.ParseFromIstream(&f)) {
            auto str = fmt::format(
                "failed to parse command from blob {}",
                NativeSupport::Unprefix(action.command_digest().hash()));
            logger_.Emit(LogLevel::Error, str);
            return {std::nullopt, str};
        }
    }
    return {c, std::nullopt};
}

static auto GetEnvVars(::bazel_re::Command const& c)
    -> std::map<std::string, std::string> {
    std::map<std::string, std::string> env_vars{};
    std::transform(c.environment_variables().begin(),
                   c.environment_variables().end(),
                   std::inserter(env_vars, env_vars.begin()),
                   [](auto const& x) -> std::pair<std::string, std::string> {
                       return {x.name(), x.value()};
                   });
    return env_vars;
}

auto ExecutionServiceImpl::GetIExecutionAction(
    ::bazel_re::ExecuteRequest const* request,
    ::bazel_re::Action const& action) const
    -> std::pair<std::optional<IExecutionAction::Ptr>,
                 std::optional<std::string>> {

    auto [c, msg_c] = GetCommand(action);
    if (!c) {
        return {std::nullopt, *msg_c};
    }

    auto env_vars = GetEnvVars(*c);

    auto i_execution_action = api_->CreateAction(
        ArtifactDigest{action.input_root_digest()},
        {c->arguments().begin(), c->arguments().end()},
        {c->output_files().begin(), c->output_files().end()},
        {c->output_directories().begin(), c->output_directories().end()},
        env_vars,
        {});
    if (!i_execution_action) {
        auto str = fmt::format(
            "could not create action from {}",
            NativeSupport::Unprefix(request->action_digest().hash()));
        logger_.Emit(LogLevel::Error, str);
        return {std::nullopt, str};
    }
    i_execution_action->SetCacheFlag(
        action.do_not_cache() ? IExecutionAction::CacheFlag::DoNotCacheOutput
                              : IExecutionAction::CacheFlag::CacheOutput);
    return {std::move(i_execution_action), std::nullopt};
}

static auto GetDirectoryFromDigest(::bazel_re::Digest const& digest,
                                   LocalStorage const& storage) noexcept
    -> std::optional<::bazel_re::Directory> {
    // determine directory path from digest
    auto const& path = storage.BlobPath(digest, /*is_executable=*/false);
    if (not path) {
        return std::nullopt;
    }

    // read directory content from path
    auto const& content = FileSystemManager::ReadFile(*path);
    if (not content) {
        return std::nullopt;
    }

    // parse directory content
    ::bazel_re::Directory dir{};
    if (not dir.ParseFromString(*content)) {
        return std::nullopt;
    }
    return dir;
}

// NOLINTNEXTLINE(misc-no-recursion)
static auto CollectChildDirectoriesRecursively(
    ::bazel_re::Directory const& root,
    LocalStorage const& storage,
    gsl::not_null<std::unordered_map<::bazel_re::Digest,
                                     ::bazel_re::Directory>*> map) noexcept
    -> bool {
    return std::all_of(root.directories().begin(),
                       root.directories().end(),
                       // NOLINTNEXTLINE(misc-no-recursion)
                       [&map, &storage](auto const& node) {
                           if (map->find(node.digest()) != map->end()) {
                               return true;
                           }
                           auto tmp_root =
                               GetDirectoryFromDigest(node.digest(), storage);
                           if (not tmp_root) {
                               return false;
                           }
                           if (not CollectChildDirectoriesRecursively(
                                   *tmp_root, storage, map)) {
                               return false;
                           }
                           try {
                               map->emplace(node.digest(), *tmp_root);
                           } catch (...) {
                               return false;
                           }
                           return true;
                       });
}

static auto GetChildrenFromDirectory(::bazel_re::Directory const& root,
                                     LocalStorage const& storage) noexcept
    -> std::optional<std::vector<::bazel_re::Directory>> {
    // determine child directories
    std::unordered_map<::bazel_re::Digest, ::bazel_re::Directory> map{};
    if (not CollectChildDirectoriesRecursively(root, storage, &map)) {
        return std::nullopt;
    }

    // extract digests from child directories
    std::vector<::bazel_re::Digest> digests{};
    digests.reserve(map.size());
    std::transform(map.begin(),
                   map.end(),
                   std::back_inserter(digests),
                   [](auto const& pair) { return pair.first; });

    // sort digests
    std::sort(digests.begin(),
              digests.end(),
              [](auto const& left, auto const& right) {
                  return left.hash() < right.hash();
              });

    // extract directory messages
    std::vector<::bazel_re::Directory> children{};
    children.reserve(digests.size());
    std::transform(digests.begin(),
                   digests.end(),
                   std::back_inserter(children),
                   [&map](auto const& digest) { return map[digest]; });

    return children;
}

static auto CreateTreeDigestFromDirectoryDigest(
    ::bazel_re::Digest const& dir_digest,
    LocalStorage const& storage) noexcept -> std::optional<::bazel_re::Digest> {
    // determine root directory message
    auto root = GetDirectoryFromDigest(dir_digest, storage);
    if (not root) {
        return std::nullopt;
    }

    // determine child directory messages
    auto children = GetChildrenFromDirectory(*root, storage);
    if (not children) {
        return std::nullopt;
    }

    // create tree message
    ::bazel_re::Tree tree{};
    tree.set_allocated_root(
        gsl::owner<::bazel_re::Directory*>{new ::bazel_re::Directory{*root}});
    tree.mutable_children()->Reserve(gsl::narrow<int>((*children).size()));
    std::copy((*children).begin(),
              (*children).end(),
              ::pb::back_inserter(tree.mutable_children()));

    // serialize and store tree message
    auto content = tree.SerializeAsString();
    auto tree_digest = storage.StoreBlob(content, /*is_executable=*/false);
    if (not tree_digest) {
        return std::nullopt;
    }

    return tree_digest;
}

static auto AddOutputPaths(::bazel_re::ExecuteResponse* response,
                           IExecutionResponse::Ptr const& execution,
                           LocalStorage const& storage) noexcept -> bool {
    auto const& size = static_cast<int>(execution->Artifacts().size());
    response->mutable_result()->mutable_output_files()->Reserve(size);
    response->mutable_result()->mutable_output_directories()->Reserve(size);

    for (auto const& [path, info] : execution->Artifacts()) {
        auto dgst = static_cast<::bazel_re::Digest>(info.digest);

        if (info.type == ObjectType::Tree) {
            ::bazel_re::OutputDirectory out_dir;
            *(out_dir.mutable_path()) = path;
            if (not Compatibility::IsCompatible()) {
                // In native mode: Set the directory digest directly.
                *(out_dir.mutable_tree_digest()) = std::move(dgst);
            }
            else {
                // In compatible mode: Create a tree digest from directory
                // digest on the fly and set tree digest.
                auto digest =
                    CreateTreeDigestFromDirectoryDigest(dgst, storage);
                if (not digest) {
                    return false;
                }
                *(out_dir.mutable_tree_digest()) = std::move(*digest);
            }
            response->mutable_result()->mutable_output_directories()->Add(
                std::move(out_dir));
        }
        else {
            ::bazel_re::OutputFile out_file;
            *(out_file.mutable_path()) = path;
            *(out_file.mutable_digest()) = std::move(dgst);
            out_file.set_is_executable(info.type == ObjectType::Executable);
            response->mutable_result()->mutable_output_files()->Add(
                std::move(out_file));
        }
    }
    return true;
}

auto ExecutionServiceImpl::AddResult(
    ::bazel_re::ExecuteResponse* response,
    IExecutionResponse::Ptr const& i_execution_response,
    std::string const& action_hash) const noexcept
    -> std::optional<std::string> {
    if (not AddOutputPaths(response, i_execution_response, storage_)) {
        auto str = fmt::format("Error in creating output paths of action {}",
                               action_hash);
        logger_.Emit(LogLevel::Error, str);
        return std::nullopt;
    }
    auto* result = response->mutable_result();
    result->set_exit_code(i_execution_response->ExitCode());
    if (i_execution_response->HasStdErr()) {
        auto dgst = storage_.StoreBlob(i_execution_response->StdErr(),
                                       /*is_executable=*/false);
        if (!dgst) {
            auto str =
                fmt::format("Could not store stderr of action {}", action_hash);
            logger_.Emit(LogLevel::Error, str);
            return str;
        }
        result->mutable_stderr_digest()->CopyFrom(*dgst);
    }
    if (i_execution_response->HasStdOut()) {
        auto dgst = storage_.StoreBlob(i_execution_response->StdOut(),
                                       /*is_executable=*/false);
        if (!dgst) {
            auto str =
                fmt::format("Could not store stdout of action {}", action_hash);
            logger_.Emit(LogLevel::Error, str);
            return str;
        }
        result->mutable_stdout_digest()->CopyFrom(*dgst);
    }
    return std::nullopt;
}

static void AddStatus(::bazel_re::ExecuteResponse* response) noexcept {
    ::google::rpc::Status status{};
    // we run the action locally, so no communication issues should happen
    status.set_code(grpc::StatusCode::OK);
    *(response->mutable_status()) = status;
}

auto ExecutionServiceImpl::GetResponse(
    ::bazel_re::ExecuteRequest const* request,
    IExecutionResponse::Ptr const& i_execution_response) const noexcept
    -> std::pair<std::optional<::bazel_re::ExecuteResponse>,
                 std::optional<std::string>> {

    ::bazel_re::ExecuteResponse response{};
    AddStatus(&response);
    auto err =
        AddResult(&response,
                  i_execution_response,
                  NativeSupport::Unprefix(request->action_digest().hash()));
    if (err) {
        return {std::nullopt, *err};
    }
    response.set_cached_result(i_execution_response->IsCached());
    return {response, std::nullopt};
}

auto ExecutionServiceImpl::WriteResponse(
    ::bazel_re::ExecuteRequest const* request,
    IExecutionResponse::Ptr const& i_execution_response,
    ::bazel_re::Action const& action,
    ::grpc::ServerWriter<::google::longrunning::Operation>* writer)
    const noexcept -> std::optional<std::string> {

    auto [execute_response, msg_r] = GetResponse(request, i_execution_response);
    if (!execute_response) {
        return *msg_r;
    }

    // store action result
    if (i_execution_response->ExitCode() == 0 && !action.do_not_cache() &&
        !storage_.StoreActionResult(request->action_digest(),
                                    execute_response->result())) {
        auto str = fmt::format(
            "Could not store action result for action {}",
            NativeSupport::Unprefix(request->action_digest().hash()));
        logger_.Emit(LogLevel::Error, str);
        return str;
    }

    // send response to the client
    auto op = ::google::longrunning::Operation{};
    op.mutable_response()->PackFrom(*execute_response);
    op.set_name("just-remote-execution");
    op.set_done(true);
    if (!writer->Write(op)) {
        auto str = fmt::format(
            "Could not write execution response for action {}",
            NativeSupport::Unprefix(request->action_digest().hash()));
        logger_.Emit(LogLevel::Error, str);
        return str;
    }
    return std::nullopt;
}

auto ExecutionServiceImpl::Execute(
    ::grpc::ServerContext* /*context*/,
    const ::bazel_re::ExecuteRequest* request,
    ::grpc::ServerWriter<::google::longrunning::Operation>* writer)
    -> ::grpc::Status {

    auto lock = GarbageCollector::SharedLock();
    if (!lock) {
        auto str = fmt::format("Could not acquire SharedLock");
        logger_.Emit(LogLevel::Error, str);
        return grpc::Status{grpc::StatusCode::INTERNAL, str};
    }
    auto [action, msg_a] = GetAction(request);
    if (!action) {
        return ::grpc::Status{grpc::StatusCode::INTERNAL, *msg_a};
    }
    auto [i_execution_action, msg] = GetIExecutionAction(request, *action);
    if (!i_execution_action) {
        return ::grpc::Status{grpc::StatusCode::INTERNAL, *msg};
    }

    logger_.Emit(LogLevel::Info,
                 "Execute {}",
                 NativeSupport::Unprefix(request->action_digest().hash()));
    auto i_execution_response = i_execution_action->get()->Execute(&logger_);
    logger_.Emit(LogLevel::Trace,
                 "Finished execution of {}",
                 NativeSupport::Unprefix(request->action_digest().hash()));
    auto err = WriteResponse(request, i_execution_response, *action, writer);
    if (err) {
        return ::grpc::Status{grpc::StatusCode::INTERNAL, *err};
    }
    return ::grpc::Status::OK;
}

auto ExecutionServiceImpl::WaitExecution(
    ::grpc::ServerContext* /*context*/,
    const ::bazel_re::WaitExecutionRequest* /*request*/,
    ::grpc::ServerWriter<::google::longrunning::Operation>* /*writer*/)
    -> ::grpc::Status {
    auto const* str = "WaitExecution not implemented";
    logger_.Emit(LogLevel::Error, str);
    return ::grpc::Status{grpc::StatusCode::UNIMPLEMENTED, str};
}