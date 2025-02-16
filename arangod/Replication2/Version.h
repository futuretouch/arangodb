////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Tobias Gödderz
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <string>
#include <string_view>

#include <ProgramOptions/Parameters.h>
#include <Basics/ResultT.h>

namespace arangodb {
template<typename T>
class ResultT;
}
namespace arangodb::velocypack {
class Slice;
}

namespace arangodb::replication {

enum class Version { ONE = 1, TWO = 2 };

constexpr inline auto allowedVersions = {Version::ONE, Version::TWO};

auto parseVersion(std::string_view version) -> ResultT<replication::Version>;
auto parseVersion(velocypack::Slice version) -> ResultT<replication::Version>;

auto versionToString(Version version) -> std::string_view;

}  // namespace arangodb::replication
