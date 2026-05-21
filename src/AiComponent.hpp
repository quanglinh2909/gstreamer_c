#ifndef test_gstreamer_AiComponent_hpp
#define test_gstreamer_AiComponent_hpp

#include "ai/AiManager.hpp"

#include "oatpp/core/macro/component.hpp"

// Registers the in-process AI subsystem so services/controllers can resolve it
// via OATPP_COMPONENT. App.cpp owns starting/stopping it.
class AiComponent {
public:
    OATPP_CREATE_COMPONENT(std::shared_ptr<AiManager>, aiManager)([] {
        return std::make_shared<AiManager>();
    }());
};

#endif
