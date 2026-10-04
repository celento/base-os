#ifndef POINTER_BACKEND_H
#define POINTER_BACKEND_H

/* Only endpoint negotiation varies. The model, event consumer, drawing and
 * event loop are shared, including RELEASE followed by an explicit reopen.
 * The builder defines BOS_APP_NATIVE_WINDOW_V1 only for --window native-v1. */
#ifdef BOS_APP_NATIVE_WINDOW_V1
#define POINTER_BACKEND_CAPABILITIES (BOS_UI_CAP_OWNED_WINDOW|BOS_UI_CAP_FORCED_CLOSE)
#define pointer_backend_open bos_ui_window_adopt
#define POINTER_BACKEND_UNSUPPORTED \
    "POINTER UNSUPPORTED: launch /Programs/pointer-window.bex on a native-window kernel.\n"
#else
#define POINTER_BACKEND_CAPABILITIES BOS_UI_CAP_HOSTED_CANVAS
#define pointer_backend_open bos_ui_host_open
#define pointer_backend_query bos_ui_query
#define pointer_backend_target_valid(info) ((info)->kind==BOS_UI_KIND_HOSTED_CANVAS)
#define POINTER_BACKEND_UNSUPPORTED \
    "POINTER UNSUPPORTED: use start /Programs/pointer.bex on a hosted-UI kernel.\n"
#endif

#define POINTER_REQUIRED_CAPABILITIES (POINTER_BACKEND_CAPABILITIES|BOS_UI_CAP_POINTER| \
    BOS_UI_CAP_IMPLICIT_CAPTURE|BOS_UI_CAP_BOUNDED_WAIT|BOS_UI_CAP_LEGACY_KEY_READINESS)

#ifdef BOS_APP_NATIVE_WINDOW_V1
static int pointer_backend_query(BosUiInfoV1 *out,unsigned capacity) {
    BosAbiInfo abi;
    const unsigned required=BOS_FEATURE_BEX2|BOS_FEATURE_OWNED_NATIVE_WINDOW;
    int result=bos_abi_query(&abi,sizeof abi);
    if(result!=BOS_OK)return result;
    if(abi.struct_size<sizeof abi||abi.abi_major!=BOS_ABI_MAJOR||abi.abi_minor<2||
       abi.context!=BOS_CONTEXT_DESKTOP_TASK||(abi.features&required)!=required)
        return BOS_E_UNSUPPORTED;
    result=bos_ui_query(out,capacity);
    if(result==BOS_OK&&out->minor<1)return BOS_E_UNSUPPORTED;
    return result;
}
static int pointer_backend_target_valid(const BosUiTargetInfoV1 *info) {
    return info->kind==BOS_UI_KIND_OWNED_WINDOW&&info->minor>=1&&
        (info->capabilities&POINTER_REQUIRED_CAPABILITIES)==POINTER_REQUIRED_CAPABILITIES;
}
#endif
#endif
