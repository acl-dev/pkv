namespace acl
{
    //pkv::pkv_node
    acl::string gson(const pkv::pkv_node &$obj);
    acl::json_node& gson(acl::json &$json, const pkv::pkv_node &$obj);
    acl::json_node& gson(acl::json &$json, const pkv::pkv_node *$obj);
    std::pair<bool,std::string> gson(acl::json_node &$node, pkv::pkv_node &$obj);
    std::pair<bool,std::string> gson(acl::json_node &$node, pkv::pkv_node *$obj);
    std::pair<bool,std::string> gson(const acl::string &str, pkv::pkv_node &$obj);

    //pkv::pkv_nodes
    acl::string gson(const pkv::pkv_nodes &$obj);
    acl::json_node& gson(acl::json &$json, const pkv::pkv_nodes &$obj);
    acl::json_node& gson(acl::json &$json, const pkv::pkv_nodes *$obj);
    std::pair<bool,std::string> gson(acl::json_node &$node, pkv::pkv_nodes &$obj);
    std::pair<bool,std::string> gson(acl::json_node &$node, pkv::pkv_nodes *$obj);
    std::pair<bool,std::string> gson(const acl::string &str, pkv::pkv_nodes &$obj);

    //pkv::slot_range
    acl::string gson(const pkv::slot_range &$obj);
    acl::json_node& gson(acl::json &$json, const pkv::slot_range &$obj);
    acl::json_node& gson(acl::json &$json, const pkv::slot_range *$obj);
    std::pair<bool,std::string> gson(acl::json_node &$node, pkv::slot_range &$obj);
    std::pair<bool,std::string> gson(acl::json_node &$node, pkv::slot_range *$obj);
    std::pair<bool,std::string> gson(const acl::string &str, pkv::slot_range &$obj);

}///end of acl.
