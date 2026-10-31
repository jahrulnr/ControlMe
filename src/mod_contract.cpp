#include "app.h"
#include "contract_store.h"

void controlme_register_contract(controlme::App& app) {
  if (app.littleFsMounted) app.contracts = controlme_install_contracts();
}
