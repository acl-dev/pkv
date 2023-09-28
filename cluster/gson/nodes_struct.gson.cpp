#include "stdafx.h"
#include "nodes_struct.h"
#include "nodes_struct.gson.h"
namespace acl
{
    acl::json_node& gson(acl::json &$json, const pkv::pkv_node &$obj)
    {
        acl::json_node &$node = $json.create_node();

        if (check_nullptr($obj.id))
            $node.add_null("id");
        else
            $node.add_text("id", acl::get_value($obj.id));

        if (check_nullptr($obj.addr))
            $node.add_null("addr");
        else
            $node.add_text("addr", acl::get_value($obj.addr));

        if (check_nullptr($obj.mana_port))
            $node.add_null("mana_port");
        else
            $node.add_number("mana_port", acl::get_value($obj.mana_port));

        if (check_nullptr($obj.type))
            $node.add_null("type");
        else
            $node.add_number("type", acl::get_value($obj.type));

        if (check_nullptr($obj.myself))
            $node.add_null("myself");
        else
            $node.add_bool("myself", acl::get_value($obj.myself));

        if (check_nullptr($obj.idx))
            $node.add_null("idx");
        else
            $node.add_number("idx", acl::get_value($obj.idx));

        if (check_nullptr($obj.join_time))
            $node.add_null("join_time");
        else
            $node.add_number("join_time", acl::get_value($obj.join_time));

        if (check_nullptr($obj.connected))
            $node.add_null("connected");
        else
            $node.add_bool("connected", acl::get_value($obj.connected));

        if (check_nullptr($obj.slots))
            $node.add_null("slots");
        else
            $node.add_child("slots", acl::gson($json, $obj.slots));


        return $node;
    }
    
    acl::json_node& gson(acl::json &$json, const pkv::pkv_node *$obj)
    {
        return gson ($json, *$obj);
    }


    acl::string gson(const pkv::pkv_node &$obj)
    {
        acl::json $json;
        acl::json_node &$node = acl::gson ($json, $obj);
        return $node.to_string ();
    }


    std::pair<bool,std::string> gson(acl::json_node &$node, pkv::pkv_node &$obj)
    {
        acl::json_node *id = $node["id"];
        acl::json_node *addr = $node["addr"];
        acl::json_node *mana_port = $node["mana_port"];
        acl::json_node *type = $node["type"];
        acl::json_node *myself = $node["myself"];
        acl::json_node *idx = $node["idx"];
        acl::json_node *join_time = $node["join_time"];
        acl::json_node *connected = $node["connected"];
        acl::json_node *slots = $node["slots"];
        std::pair<bool, std::string> $result;

        if(!id ||!($result = gson(*id, &$obj.id), $result.first))
            return std::make_pair(false, "required [pkv::pkv_node.id] failed:{"+$result.second+"}");
     
        if(!addr ||!($result = gson(*addr, &$obj.addr), $result.first))
            return std::make_pair(false, "required [pkv::pkv_node.addr] failed:{"+$result.second+"}");
     
        if(!mana_port ||!($result = gson(*mana_port, &$obj.mana_port), $result.first))
            return std::make_pair(false, "required [pkv::pkv_node.mana_port] failed:{"+$result.second+"}");
     
        if(!type ||!($result = gson(*type, &$obj.type), $result.first))
            return std::make_pair(false, "required [pkv::pkv_node.type] failed:{"+$result.second+"}");
     
        if(!myself ||!($result = gson(*myself, &$obj.myself), $result.first))
            return std::make_pair(false, "required [pkv::pkv_node.myself] failed:{"+$result.second+"}");
     
        if(!idx ||!($result = gson(*idx, &$obj.idx), $result.first))
            return std::make_pair(false, "required [pkv::pkv_node.idx] failed:{"+$result.second+"}");
     
        if(!join_time ||!($result = gson(*join_time, &$obj.join_time), $result.first))
            return std::make_pair(false, "required [pkv::pkv_node.join_time] failed:{"+$result.second+"}");
     
        if(!connected ||!($result = gson(*connected, &$obj.connected), $result.first))
            return std::make_pair(false, "required [pkv::pkv_node.connected] failed:{"+$result.second+"}");
     
        if(!slots ||!slots->get_obj()||!($result = gson(*slots->get_obj(), &$obj.slots), $result.first))
            return std::make_pair(false, "required [pkv::pkv_node.slots] failed:{"+$result.second+"}");
     
        return std::make_pair(true,"");
    }


    std::pair<bool,std::string> gson(acl::json_node &$node, pkv::pkv_node *$obj)
    {
        return gson($node, *$obj);
    }


     std::pair<bool,std::string> gson(const acl::string &$str, pkv::pkv_node &$obj)
    {
        acl::json _json;
        _json.update($str.c_str());
        if (!_json.finish())
        {
            return std::make_pair(false, "json not finish error");
        }
        return gson(_json.get_root(), $obj);
    }


    acl::json_node& gson(acl::json &$json, const pkv::pkv_nodes &$obj)
    {
        acl::json_node &$node = $json.create_node();

        if (check_nullptr($obj.nodes))
            $node.add_null("nodes");
        else
            $node.add_child("nodes", acl::gson($json, $obj.nodes));


        return $node;
    }
    
    acl::json_node& gson(acl::json &$json, const pkv::pkv_nodes *$obj)
    {
        return gson ($json, *$obj);
    }


    acl::string gson(const pkv::pkv_nodes &$obj)
    {
        acl::json $json;
        acl::json_node &$node = acl::gson ($json, $obj);
        return $node.to_string ();
    }


    std::pair<bool,std::string> gson(acl::json_node &$node, pkv::pkv_nodes &$obj)
    {
        acl::json_node *nodes = $node["nodes"];
        std::pair<bool, std::string> $result;

        if(!nodes ||!nodes->get_obj()||!($result = gson(*nodes->get_obj(), &$obj.nodes), $result.first))
            return std::make_pair(false, "required [pkv::pkv_nodes.nodes] failed:{"+$result.second+"}");
     
        return std::make_pair(true,"");
    }


    std::pair<bool,std::string> gson(acl::json_node &$node, pkv::pkv_nodes *$obj)
    {
        return gson($node, *$obj);
    }


     std::pair<bool,std::string> gson(const acl::string &$str, pkv::pkv_nodes &$obj)
    {
        acl::json _json;
        _json.update($str.c_str());
        if (!_json.finish())
        {
            return std::make_pair(false, "json not finish error");
        }
        return gson(_json.get_root(), $obj);
    }


    acl::json_node& gson(acl::json &$json, const pkv::slot_range &$obj)
    {
        acl::json_node &$node = $json.create_node();

        if (check_nullptr($obj.begin))
            $node.add_null("begin");
        else
            $node.add_number("begin", acl::get_value($obj.begin));

        if (check_nullptr($obj.end))
            $node.add_null("end");
        else
            $node.add_number("end", acl::get_value($obj.end));


        return $node;
    }
    
    acl::json_node& gson(acl::json &$json, const pkv::slot_range *$obj)
    {
        return gson ($json, *$obj);
    }


    acl::string gson(const pkv::slot_range &$obj)
    {
        acl::json $json;
        acl::json_node &$node = acl::gson ($json, $obj);
        return $node.to_string ();
    }


    std::pair<bool,std::string> gson(acl::json_node &$node, pkv::slot_range &$obj)
    {
        acl::json_node *begin = $node["begin"];
        acl::json_node *end = $node["end"];
        std::pair<bool, std::string> $result;

        if(!begin ||!($result = gson(*begin, &$obj.begin), $result.first))
            return std::make_pair(false, "required [pkv::slot_range.begin] failed:{"+$result.second+"}");
     
        if(!end ||!($result = gson(*end, &$obj.end), $result.first))
            return std::make_pair(false, "required [pkv::slot_range.end] failed:{"+$result.second+"}");
     
        return std::make_pair(true,"");
    }


    std::pair<bool,std::string> gson(acl::json_node &$node, pkv::slot_range *$obj)
    {
        return gson($node, *$obj);
    }


     std::pair<bool,std::string> gson(const acl::string &$str, pkv::slot_range &$obj)
    {
        acl::json _json;
        _json.update($str.c_str());
        if (!_json.finish())
        {
            return std::make_pair(false, "json not finish error");
        }
        return gson(_json.get_root(), $obj);
    }


}///end of acl.
