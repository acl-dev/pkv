#pragma once
#include "acl_policy.h"
#include "command_catalog.h"

namespace pkv {
const command_catalog_entry* acl_lookup(const std::string& lower_name);
bool acl_has_children(const std::string& lower_name);
bool acl_split(const std::string& text, std::vector<std::string>& out);
std::string acl_quote(const std::string& text);
const std::vector<const command_catalog_entry*>& acl_catalog();
std::string acl_selector_rule(acl_selector& selector, const std::string& rule);
} // namespace pkv
