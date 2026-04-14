#pragma once

#include "duckdb.hpp"

namespace duckdb {

// Main extension loader
class SharepointExtension : public Extension {
public:
	void Load(ExtensionLoader &db) override;
	std::string Name() override;
	std::string Version() const override;
};

// The DuckDB build system derives the linked static extension class name from the
// extension target name. Keep this alias so the implementation can stay on the
// existing SharepointExtension class while the packaged extension name is spxlsx.
using SpxlsxExtension = SharepointExtension;

} // namespace duckdb
