#ifndef BASEOS_NATIVE_UI_H
#define BASEOS_NATIVE_UI_H
#include "program.h"
#include "canvas_view.h"
#include "input_ingress.h"
#include "../sdk/baseos_abi.h"
/* Serialized kernel/desktop service only. No callback or application pointer
 * is retained. These hooks are trusted fixed desktop functions, never users. */
#define NATIVE_UI_CAPABILITIES_COMMON (BOS_UI_CAP_POINTER|BOS_UI_CAP_HOVER|BOS_UI_CAP_WHEEL|BOS_UI_CAP_IMPLICIT_CAPTURE|BOS_UI_CAP_BOUNDED_WAIT|BOS_UI_CAP_LEGACY_KEY_READINESS)
#define NATIVE_UI_CAPABILITIES_HOSTED (NATIVE_UI_CAPABILITIES_COMMON|BOS_UI_CAP_HOSTED_CANVAS|BOS_UI_CAP_HOST_FORCED_CLOSE)
#define NATIVE_UI_CAPABILITIES_OWNED (NATIVE_UI_CAPABILITIES_COMMON|BOS_UI_CAP_OWNED_WINDOW|BOS_UI_CAP_FORCED_CLOSE)
/* Backend identity is trusted desktop metadata, never supplied by an app. */
typedef struct { CanvasView view; unsigned state,kind,capabilities; } NativeUiHost;
typedef struct { uint64_t serial; unsigned buttons,ticks; } NativeUiAcquired;
typedef struct {
    /* Must check a live process AND its full copied app-view binding. */
    int (*snapshot)(const ProcessBinding *,NativeUiHost *);
    void (*acquired)(NativeUiAcquired *);
    void (*focus)(const ProcessBinding *);
} NativeUiHooks;
void native_ui_init(const NativeUiHooks *hooks);
int native_ui_available(void);
void native_ui_query(BosUiInfoV1 *out,unsigned ticks_per_second);
void native_ui_query_kind(BosUiInfoV1 *out,unsigned ticks_per_second,unsigned kind);
int native_ui_open(const ProcessBinding *owner,unsigned subscriptions,BosUiTargetInfoV1 *out);
/* ADOPT only subscribes to an already-owned primary window. */
int native_ui_adopt(const ProcessBinding *owner,unsigned subscriptions,BosUiTargetInfoV1 *out);
int native_ui_info(const ProcessBinding *owner,BosHandle target,BosUiTargetInfoV1 *out);
int native_ui_read(const ProcessBinding *owner,BosHandle target,BosUiEventV1 *out);
int native_ui_ready(const ProcessBinding *owner,BosHandle target);
int native_ui_release(const ProcessBinding *owner,BosHandle target);
/* Revoke immediately at Stop/exit; final owner cleanup is safe and idempotent. */
void native_ui_revoke_owner(ProcessHandle owner);
void native_ui_release_owner(ProcessHandle owner);
/* For WM hit-testing only: never expose this lookup or a slot in the SDK. */
BosHandle native_ui_target_at(unsigned slot);
void native_ui_refresh(unsigned ticks,unsigned physical_buttons);
void native_ui_cancel_all(unsigned reason,unsigned ticks,unsigned physical_buttons);
void native_ui_input_loss(unsigned ticks,unsigned physical_buttons,unsigned dropped);
#define NATIVE_UI_CONSUMED_POINTER 1u
#define NATIVE_UI_CONSUMED_WHEEL 2u
/* hit_target comes from the sole front-to-back WM point test. Zero means chrome,
 * overlay, padding, another app, or no published canvas. Capture overrides it. */
unsigned native_ui_route(const InputSample *sample,BosHandle hit_target);
#endif
