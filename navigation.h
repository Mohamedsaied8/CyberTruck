#include <string>

class Navigation {
public:
    Navigation() = default;
    void navigateTo(const std::string& destination);
    void setCurrentLocation(const std::string& location);
    std::string getCurrentLocation() const;
};
