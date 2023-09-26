//
// Created by shuxin 　　zheng on 2023/7/27
//

#pragma once

#if __cplusplus >=  201703L
# define NODISCARD      [[nodiscard]]
# define NORETURN       [[noreturn]]
#else
# define NODISCARD
# define NORETURN
#endif
