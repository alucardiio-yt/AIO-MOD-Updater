#pragma once

namespace app_update_cleanup {
enum class Result { done, appRecovery, bootRecovery, cfwPending };

bool pending();
Result clean(bool cfwPending);
Result recoverBoot(bool cfwPending);
}
