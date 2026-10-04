#ifndef NATIVE_UI_SERVICE_STUBS_H
#define NATIVE_UI_SERVICE_STUBS_H
#include "native_ui.h"
#ifndef NATIVE_UI_REAL_SERVICE
/* Existing dispatcher/lifecycle fixtures do not configure a hosted UI. The
 * dedicated UI dispatcher fixture links the real core and supplies WM hooks. */
int native_ui_available(void){return 0;}
void native_ui_query(BosUiInfoV1 *out,unsigned hz){(void)out;(void)hz;}
int native_ui_open(const ProcessBinding *b,unsigned s,BosUiTargetInfoV1 *out){(void)b;(void)s;(void)out;return BOS_E_UNSUPPORTED;}
int native_ui_info(const ProcessBinding *b,BosHandle h,BosUiTargetInfoV1 *out){(void)b;(void)h;(void)out;return BOS_E_STALE;}
int native_ui_read(const ProcessBinding *b,BosHandle h,BosUiEventV1 *out){(void)b;(void)h;(void)out;return BOS_E_STALE;}
int native_ui_ready(const ProcessBinding *b,BosHandle h){(void)b;(void)h;return BOS_E_STALE;}
int native_ui_release(const ProcessBinding *b,BosHandle h){(void)b;(void)h;return BOS_E_STALE;}
void native_ui_revoke_owner(ProcessHandle owner){(void)owner;}
void native_ui_release_owner(ProcessHandle owner){(void)owner;}
#endif
#endif
