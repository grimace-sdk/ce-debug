#ifndef DEBUGMGR_BRAIN_DEVICE_H
#define DEBUGMGR_BRAIN_DEVICE_H

#include "device_ce.h"

namespace debugmgr {

class brain_device : public ce_device {
public:
    uint32_t screenshot(const std::u16string &path, uint32_t &width, uint32_t &height) override;
};

}

#endif
