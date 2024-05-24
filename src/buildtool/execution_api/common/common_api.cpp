// Copyright 2024 Huawei Cloud Computing Technology Co., Ltd.
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

#include "src/buildtool/execution_api/common/common_api.hpp"

#include <cstddef>
#include <exception>

#include "fmt/core.h"
#include "src/buildtool/logging/log_level.hpp"
#include "src/buildtool/logging/logger.hpp"

auto CommonRetrieveToFds(
    std::vector<Artifact::ObjectInfo> const& artifacts_info,
    std::vector<int> const& fds,
    std::function<bool(Artifact::ObjectInfo const&,
                       gsl::not_null<FILE*> const&)> const& dump_to_stream,
    std::optional<std::function<bool(Artifact::ObjectInfo const&, int)>> const&
        fallback) noexcept -> bool {
    if (artifacts_info.size() != fds.size()) {
        Logger::Log(LogLevel::Error,
                    "different number of digests and file descriptors.");
        return false;
    }

    for (std::size_t i{}; i < artifacts_info.size(); ++i) {
        auto fd = fds[i];
        auto const& info = artifacts_info[i];

        if (gsl::owner<FILE*> out = fdopen(dup(fd), "wb")) {  // NOLINT
            bool success{false};
            try {
                success = dump_to_stream(info, out);
            } catch (std::exception const& ex) {
                std::fclose(out);  // close file
                Logger::Log(LogLevel::Error,
                            "dumping {} to stream failed with:\n{}",
                            info.ToString(),
                            ex.what());
                return false;
            }
            std::fclose(out);
            if (not success) {
                Logger::Log(
                    LogLevel::Debug,
                    "dumping {} {} from CAS to file descriptor {} failed.",
                    IsTreeObject(info.type) ? "tree" : "blob",
                    info.ToString(),
                    fd);
                // locally we might be able to fallback to Git in native mode
                try {
                    if (fallback and not(*fallback)(info, fd)) {
                        return false;
                    }
                } catch (std::exception const& ex) {
                    Logger::Log(LogLevel::Error,
                                "fallback dumping {} to file descriptor {} "
                                "failed with:\n{}",
                                info.ToString(),
                                fd,
                                ex.what());
                    return false;
                }
            }
        }
        else {
            Logger::Log(
                LogLevel::Error, "dumping to file descriptor {} failed.", fd);
            return false;
        }
    }
    return true;
}

/// NOLINTNEXTLINE(misc-no-recursion)
auto CommonUploadBlobTree(BlobTreePtr const& blob_tree,
                          gsl::not_null<IExecutionApi*> const& api) noexcept
    -> bool {
    // Create digest list from blobs for batch availability check.
    auto missing_blobs_info = GetMissingArtifactsInfo<BlobTreePtr>(
        api, blob_tree->begin(), blob_tree->end(), [](BlobTreePtr const& node) {
            return ArtifactDigest{node->Blob().digest};
        });
    if (not missing_blobs_info) {
        Logger::Log(LogLevel::Error,
                    "Failed to retrieve the missing tree blobs for upload");
        return false;
    }

    // Process missing blobs.
    BazelBlobContainer container;
    for (auto const& digest : missing_blobs_info->digests) {
        if (auto it = missing_blobs_info->back_map.find(digest);
            it != missing_blobs_info->back_map.end()) {
            auto const& node = it->second;
            // Process trees.
            if (node->IsTree()) {
                if (not CommonUploadBlobTree(node, api)) {
                    return false;
                }
            }
            // Store blob.
            try {
                container.Emplace(node->Blob());
            } catch (...) {
                return false;
            }
        }
    }

    return api->Upload(container, /*skip_find_missing=*/true);
}

auto CommonUploadTreeCompatible(
    gsl::not_null<IExecutionApi*> const& api,
    DirectoryTreePtr const& build_root,
    BazelMsgFactory::LinkDigestResolveFunc const& resolve_links) noexcept
    -> std::optional<ArtifactDigest> {
    BazelBlobContainer blobs{};
    auto digest = BazelMsgFactory::CreateDirectoryDigestFromTree(
        build_root, resolve_links, [&blobs](BazelBlob&& blob) {
            blobs.Emplace(std::move(blob));
        });
    if (not digest) {
        Logger::Log(LogLevel::Debug, "failed to create digest for build root.");
        return std::nullopt;
    }
    Logger::Log(LogLevel::Trace, [&digest]() {
        std::ostringstream oss{};
        oss << "upload root directory" << std::endl;
        oss << fmt::format(" - root digest: {}", digest->hash()) << std::endl;
        return oss.str();
    });
    if (not api->Upload(blobs, /*skip_find_missing=*/false)) {
        Logger::Log(LogLevel::Debug, "failed to upload blobs for build root.");
        return std::nullopt;
    }
    return ArtifactDigest{*digest};
}

auto CommonUploadTreeNative(gsl::not_null<IExecutionApi*> const& api,
                            DirectoryTreePtr const& build_root) noexcept
    -> std::optional<ArtifactDigest> {
    auto blob_tree = BlobTree::FromDirectoryTree(build_root);
    if (not blob_tree) {
        Logger::Log(LogLevel::Debug,
                    "failed to create blob tree for build root.");
        return std::nullopt;
    }
    auto tree_blob = (*blob_tree)->Blob();
    // Upload blob tree if tree is not available at the remote side (content
    // first).
    if (not api->IsAvailable(ArtifactDigest{tree_blob.digest})) {
        if (not CommonUploadBlobTree(*blob_tree, api)) {
            Logger::Log(LogLevel::Debug,
                        "failed to upload blob tree for build root.");
            return std::nullopt;
        }
        if (not api->Upload(BazelBlobContainer{{tree_blob}},
                            /*skip_find_missing=*/true)) {
            Logger::Log(LogLevel::Debug,
                        "failed to upload tree blob for build root.");
            return std::nullopt;
        }
    }
    return ArtifactDigest{tree_blob.digest};
}
