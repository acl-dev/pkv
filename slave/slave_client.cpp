//
// Created by shuxin ¡¡¡¡zheng on 2023/9/26.
//

#include "stdafx.h"
#include "slave_client.h"

namespace pkv {

slave_client::slave_client(acl::shared_stream conn) : conn_(conn) {}
slave_client::~slave_client() {}

void slave_client::run() {
}

} // namespace pkv
