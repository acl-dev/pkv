#include "redis_geo.h"
#include <iomanip>
#include <locale>
#include <sstream>

namespace pkv {
namespace geo {
std::string coordinate_error(const point& value) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << "ERR invalid longitude,latitude pair " << std::fixed <<
        std::setprecision(6) << value.longitude << ',' << value.latitude;
    return text.str();
}
} // namespace geo
} // namespace pkv
