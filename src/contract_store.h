#pragma once
#include "ports/contract.h"

// Install bundled documents into LittleFS; call after mounting at startup.
controlme::IContractStore* controlme_install_contracts();
