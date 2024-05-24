// Copyright 2022 Huawei Cloud Computing Technology Co., Ltd.
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

#include "src/buildtool/execution_api/remote/bazel/bazel_network.hpp"

#include <algorithm>
#include <cstddef>

#include "src/buildtool/common/remote/client_common.hpp"
#include "src/buildtool/execution_api/common/message_limits.hpp"
#include "src/buildtool/execution_api/remote/bazel/bazel_response.hpp"
#include "src/buildtool/logging/log_level.hpp"
#include "src/buildtool/logging/logger.hpp"

namespace {

[[nodiscard]] auto ReadDirectory(
    gsl::not_null<BazelNetwork const*> const& network,
    bazel_re::Digest const& digest) noexcept
    -> std::optional<bazel_re::Directory> {
    auto blobs = network->ReadBlobs({digest}).Next();
    if (blobs.size() == 1) {
        return BazelMsgFactory::MessageFromString<bazel_re::Directory>(
            blobs.at(0).data);
    }
    Logger::Log(LogLevel::Debug,
                "Directory {} not found in CAS",
                NativeSupport::Unprefix(digest.hash()));
    return std::nullopt;
}

[[nodiscard]] auto ReadGitTree(
    gsl::not_null<BazelNetwork const*> const& network,
    bazel_re::Digest const& digest) noexcept
    -> std::optional<GitRepo::tree_entries_t> {
    auto blobs = network->ReadBlobs({digest}).Next();
    if (blobs.size() == 1) {
        auto const& content = blobs.at(0).data;
        auto check_symlinks =
            [&network](std::vector<bazel_re::Digest> const& ids) {
                auto size = ids.size();
                auto reader = network->ReadBlobs(ids);
                auto blobs = reader.Next();
                std::size_t count{};
                while (not blobs.empty()) {
                    if (count + blobs.size() > size) {
                        Logger::Log(LogLevel::Debug,
                                    "received more blobs than requested.");
                        return false;
                    }
                    for (auto const& blob : blobs) {
                        if (not PathIsNonUpwards(blob.data)) {
                            return false;
                        }
                    }
                    count += blobs.size();
                    blobs = reader.Next();
                }
                return true;
            };
        return GitRepo::ReadTreeData(
            content,
            HashFunction::ComputeTreeHash(content).Bytes(),
            check_symlinks,
            /*is_hex_id=*/false);
    }
    Logger::Log(LogLevel::Debug,
                "Tree {} not found in CAS",
                NativeSupport::Unprefix(digest.hash()));
    return std::nullopt;
}

[[nodiscard]] auto TreeToStream(
    gsl::not_null<BazelNetwork const*> const& network,
    bazel_re::Digest const& tree_digest,
    gsl::not_null<FILE*> const& stream,
    bool raw_tree) noexcept -> bool {
    if (raw_tree) {
        auto blobs = network->ReadBlobs({tree_digest}).Next();
        if (blobs.size() != 1) {
            Logger::Log(LogLevel::Debug,
                        "Object {} not found in CAS",
                        NativeSupport::Unprefix(tree_digest.hash()));
            return false;
        }
        auto const& str = blobs.at(0).data;
        std::fwrite(str.data(), 1, str.size(), stream);
        return true;
    }
    if (Compatibility::IsCompatible()) {
        if (auto dir = ReadDirectory(network, tree_digest)) {
            if (auto data = BazelMsgFactory::DirectoryToString(*dir)) {
                auto const& str = *data;
                std::fwrite(str.data(), 1, str.size(), stream);
                return true;
            }
        }
    }
    else {
        if (auto entries = ReadGitTree(network, tree_digest)) {
            if (auto data = BazelMsgFactory::GitTreeToString(*entries)) {
                auto const& str = *data;
                std::fwrite(str.data(), 1, str.size(), stream);
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] auto BlobToStream(
    gsl::not_null<BazelNetwork const*> const& network,
    bazel_re::Digest const& blob_digest,
    gsl::not_null<FILE*> const& stream) noexcept -> bool {
    auto reader = network->IncrementalReadSingleBlob(blob_digest);
    auto data = reader.Next();
    while (data and not data->empty()) {
        auto const& str = *data;
        std::fwrite(str.data(), 1, str.size(), stream);
        data = reader.Next();
    }
    return data.has_value();
}

}  // namespace

BazelNetwork::BazelNetwork(std::string instance_name,
                           std::string const& host,
                           Port port,
                           ExecutionConfiguration const& exec_config) noexcept
    : instance_name_{std::move(instance_name)},
      exec_config_{exec_config},
      cas_{std::make_unique<BazelCasClient>(host, port)},
      ac_{std::make_unique<BazelAcClient>(host, port)},
      exec_{std::make_unique<BazelExecutionClient>(host, port)} {}

auto BazelNetwork::IsAvailable(bazel_re::Digest const& digest) const noexcept
    -> bool {
    return cas_
        ->FindMissingBlobs(instance_name_,
                           std::vector<bazel_re::Digest>{digest})
        .empty();
}

auto BazelNetwork::IsAvailable(std::vector<bazel_re::Digest> const& digests)
    const noexcept -> std::vector<bazel_re::Digest> {
    return cas_->FindMissingBlobs(instance_name_, digests);
}

auto BazelNetwork::SplitBlob(bazel_re::Digest const& blob_digest) const noexcept
    -> std::optional<std::vector<bazel_re::Digest>> {
    return cas_->SplitBlob(instance_name_, blob_digest);
}

auto BazelNetwork::SpliceBlob(
    bazel_re::Digest const& blob_digest,
    std::vector<bazel_re::Digest> const& chunk_digests) const noexcept
    -> std::optional<bazel_re::Digest> {
    return cas_->SpliceBlob(instance_name_, blob_digest, chunk_digests);
}

auto BazelNetwork::BlobSplitSupport() const noexcept -> bool {
    return cas_->BlobSplitSupport(instance_name_);
}

auto BazelNetwork::BlobSpliceSupport() const noexcept -> bool {
    return cas_->BlobSpliceSupport(instance_name_);
}

template <class T_Iter>
auto BazelNetwork::DoUploadBlobs(T_Iter const& first,
                                 T_Iter const& last) noexcept -> bool {
    try {
        // Partition the blobs according to their size. The first group collects
        // all the blobs that can be uploaded in batch, the second group gathers
        // blobs whose size exceeds the kMaxBatchTransferSize threshold.
        //
        // The blobs belonging to the second group are uploaded via the
        // bytestream api.
        std::vector<gsl::not_null<BazelBlob const*>> sorted;
        sorted.reserve(std::distance(first, last));
        std::transform(
            first, last, std::back_inserter(sorted), [](BazelBlob const& b) {
                return &b;
            });

        auto it = std::stable_partition(
            sorted.begin(), sorted.end(), [](BazelBlob const* x) {
                return x->data.size() <= kMaxBatchTransferSize;
            });
        auto digests_count =
            cas_->BatchUpdateBlobs(instance_name_, sorted.begin(), it);

        return digests_count == std::distance(sorted.begin(), it) &&
               std::all_of(it, sorted.end(), [this](BazelBlob const* x) {
                   return cas_->UpdateSingleBlob(instance_name_, *x);
               });
    } catch (...) {
        Logger::Log(LogLevel::Warning, "Unknown exception");
        return false;
    }
}

auto BazelNetwork::UploadBlobs(BlobContainer const& blobs,
                               bool skip_find_missing) noexcept -> bool {
    if (skip_find_missing) {
        return DoUploadBlobs(blobs.begin(), blobs.end());
    }

    // find digests of blobs missing in CAS
    auto missing_digests =
        cas_->FindMissingBlobs(instance_name_, blobs.Digests());

    if (not missing_digests.empty()) {
        // update missing blobs
        auto missing_blobs = blobs.RelatedBlobs(missing_digests);
        return DoUploadBlobs(missing_blobs.begin(), missing_blobs.end());
    }
    return true;
}

auto BazelNetwork::ExecuteBazelActionSync(
    bazel_re::Digest const& action) noexcept
    -> std::optional<BazelExecutionClient::ExecutionOutput> {
    auto response =
        exec_->Execute(instance_name_, action, exec_config_, true /*wait*/);

    if (response.state !=
            BazelExecutionClient::ExecutionResponse::State::Finished or
        not response.output) {
        Logger::Log(LogLevel::Warning,
                    "Failed to execute action with execution id {}.",
                    action.hash());
        return std::nullopt;
    }

    return response.output;
}

auto BazelNetwork::BlobReader::Next() noexcept -> std::vector<BazelBlob> {
    std::size_t size{};
    std::vector<BazelBlob> blobs{};

    try {
        while (current_ != ids_.end()) {
            auto blob_size = gsl::narrow<std::size_t>(current_->size_bytes());
            size += blob_size;
            // read if size is 0 (unknown) or exceeds transfer size
            if (blob_size == 0 or size > kMaxBatchTransferSize) {
                // perform read of range [begin_, current_)
                if (begin_ == current_) {
                    auto blob = cas_->ReadSingleBlob(instance_name_, *begin_);
                    if (blob) {
                        blobs.emplace_back(std::move(*blob));
                    }
                    ++current_;
                }
                else {
                    blobs =
                        cas_->BatchReadBlobs(instance_name_, begin_, current_);
                }
                begin_ = current_;
                break;
            }
            ++current_;
        }

        if (begin_ != current_) {
            blobs = cas_->BatchReadBlobs(instance_name_, begin_, current_);
            begin_ = current_;
        }
    } catch (std::exception const& e) {
        Logger::Log(
            LogLevel::Warning, "Reading blobs failed with: {}", e.what());
        Ensures(false);
    }

    return blobs;
}

auto BazelNetwork::ReadBlobs(std::vector<bazel_re::Digest> ids) const noexcept
    -> BlobReader {
    return BlobReader{instance_name_, cas_.get(), std::move(ids)};
}

auto BazelNetwork::IncrementalReadSingleBlob(bazel_re::Digest const& id)
    const noexcept -> ByteStreamClient::IncrementalReader {
    return cas_->IncrementalReadSingleBlob(instance_name_, id);
}

auto BazelNetwork::GetCachedActionResult(
    bazel_re::Digest const& action,
    std::vector<std::string> const& output_files) const noexcept
    -> std::optional<bazel_re::ActionResult> {
    return ac_->GetActionResult(
        instance_name_, action, false, false, output_files);
}

auto BazelNetwork::QueryFullTree(bazel_re::Digest const& digest) const noexcept
    -> std::optional<std::vector<bazel_re::Directory>> {
    if (not Compatibility::IsCompatible()) {
        return std::nullopt;
    }
    return cas_->GetTree(instance_name_, digest, kMaxBatchTransferSize);
}

auto BazelNetwork::DumpToStream(Artifact::ObjectInfo const& info,
                                gsl::not_null<FILE*> const& stream,
                                bool raw_tree) const noexcept -> bool {
    return IsTreeObject(info.type)
               ? TreeToStream(this, info.digest, stream, raw_tree)
               : BlobToStream(this, info.digest, stream);
}
