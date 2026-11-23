use android_activity::AndroidApp;
use jni::objects::{JObject, JValue};
use jni::JavaVM;
use log::{error, info};

pub fn request_storage_permissions(app: &AndroidApp) {
    let vm_ptr = app.vm_as_ptr();
    let activity_ptr = app.activity_as_ptr();

    if vm_ptr.is_null() || activity_ptr.is_null() {
        error!("JavaVM or Activity pointer is null");
        return;
    }

    unsafe {
        let vm = match JavaVM::from_raw(vm_ptr.cast()) {
            Ok(v) => v,
            Err(e) => {
                error!("Failed to get JavaVM: {:?}", e);
                return;
            }
        };

        let mut env = match vm.attach_current_thread() {
            Ok(e) => e,
            Err(e) => {
                error!("Failed to attach thread to JavaVM: {:?}", e);
                return;
            }
        };

        let activity = JObject::from_raw(activity_ptr.cast());

        info!("Requesting Android Storage Permissions via JNI...");

        // 1. Check Android SDK version
        let version_class = match env.find_class("android/os/Build$VERSION") {
            Ok(c) => c,
            Err(e) => {
                error!("Failed to find Build$VERSION: {:?}", e);
                return;
            }
        };

        let sdk_int = match env.get_static_field(version_class, "SDK_INT", "I") {
            Ok(val) => val.i().unwrap_or(0),
            Err(e) => {
                error!("Failed to get SDK_INT: {:?}", e);
                0
            }
        };

        info!("Running on Android SDK_INT: {}", sdk_int);

        // 2. Android 11+ (API 30+) MANAGE_EXTERNAL_STORAGE (All Files Access)
        if sdk_int >= 30 {
            if let Ok(env_class) = env.find_class("android/os/Environment") {
                if let Ok(is_manager) = env
                    .call_static_method(env_class, "isExternalStorageManager", "()Z", &[])
                    .and_then(|v| v.z())
                {
                    info!("Environment.isExternalStorageManager() = {}", is_manager);
                    if !is_manager {
                        info!("Prompting user for All Files Access permission...");
                        // Try Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION with package URI
                        let pkg_name = "io.github.mininxd.midnightclub3remixrecomp";
                        let uri_str = format!("package:{}", pkg_name);

                        let mut launched = false;
                        if let Ok(uri_class) = env.find_class("android/net/Uri") {
                            if let Ok(j_uri_str) = env.new_string(&uri_str) {
                                if let Ok(parsed_uri) = env.call_static_method(
                                    uri_class,
                                    "parse",
                                    "(Ljava/lang/String;)Landroid/net/Uri;",
                                    &[JValue::Object(&j_uri_str)],
                                ) {
                                    if let Ok(uri_obj) = parsed_uri.l() {
                                        let action = "android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION";
                                        if let Ok(j_action) = env.new_string(action) {
                                            if let Ok(intent_class) =
                                                env.find_class("android/content/Intent")
                                            {
                                                if let Ok(intent_obj) = env.new_object(
                                                    intent_class,
                                                    "(Ljava/lang/String;Landroid/net/Uri;)V",
                                                    &[
                                                        JValue::Object(&j_action),
                                                        JValue::Object(&uri_obj),
                                                    ],
                                                ) {
                                                    let _ = env.call_method(
                                                        &activity,
                                                        "startActivity",
                                                        "(Landroid/content/Intent;)V",
                                                        &[JValue::Object(&intent_obj)],
                                                    );
                                                    if env.exception_check().unwrap_or(false) {
                                                        let _ = env.exception_clear();
                                                    } else {
                                                        launched = true;
                                                        info!("Launched MANAGE_APP_ALL_FILES_ACCESS_PERMISSION intent");
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // Fallback to general ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION
                        if !launched {
                            let action = "android.settings.MANAGE_ALL_FILES_ACCESS_PERMISSION";
                            if let Ok(j_action) = env.new_string(action) {
                                if let Ok(intent_class) = env.find_class("android/content/Intent") {
                                    if let Ok(intent_obj) = env.new_object(
                                        intent_class,
                                        "(Ljava/lang/String;)V",
                                        &[JValue::Object(&j_action)],
                                    ) {
                                        let _ = env.call_method(
                                            &activity,
                                            "startActivity",
                                            "(Landroid/content/Intent;)V",
                                            &[JValue::Object(&intent_obj)],
                                        );
                                        if env.exception_check().unwrap_or(false) {
                                            let _ = env.exception_clear();
                                        } else {
                                            info!("Launched general MANAGE_ALL_FILES_ACCESS_PERMISSION intent");
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        if env.exception_check().unwrap_or(false) {
            let _ = env.exception_clear();
        }

        // 3. Request standard READ_EXTERNAL_STORAGE & WRITE_EXTERNAL_STORAGE
        let perm1 = "android.permission.READ_EXTERNAL_STORAGE";
        let perm2 = "android.permission.WRITE_EXTERNAL_STORAGE";
        if let (Ok(j_p1), Ok(j_p2)) = (env.new_string(perm1), env.new_string(perm2)) {
            if let Ok(string_class) = env.find_class("java/lang/String") {
                if let Ok(perm_array) = env.new_object_array(2, string_class, JObject::null()) {
                    let _ = env.set_object_array_element(&perm_array, 0, &j_p1);
                    let _ = env.set_object_array_element(&perm_array, 1, &j_p2);

                    let res = env.call_method(
                        &activity,
                        "requestPermissions",
                        "([Ljava/lang/String;I)V",
                        &[JValue::Object(&perm_array), JValue::Int(1001)],
                    );
                    if env.exception_check().unwrap_or(false) {
                        let _ = env.exception_clear();
                    } else if res.is_ok() {
                        info!("Successfully prompted requestPermissions for external storage");
                    } else {
                        error!("requestPermissions call returned error: {:?}", res);
                    }
                }
            }
        }
        if env.exception_check().unwrap_or(false) {
            let _ = env.exception_clear();
        }
    }
}

pub fn open_system_file_picker(app: &AndroidApp) {
    let vm_ptr = app.vm_as_ptr();
    let activity_ptr = app.activity_as_ptr();

    if vm_ptr.is_null() || activity_ptr.is_null() {
        return;
    }

    unsafe {
        let vm = match JavaVM::from_raw(vm_ptr.cast()) {
            Ok(v) => v,
            Err(_) => return,
        };

        let mut env = match vm.attach_current_thread() {
            Ok(e) => e,
            Err(_) => return,
        };

        let activity = JObject::from_raw(activity_ptr.cast());

        info!("Opening Android System File Picker for ISO selection...");

        if let Ok(intent_class) = env.find_class("android/content/Intent") {
            let action = "android.intent.action.OPEN_DOCUMENT";
            if let Ok(j_action) = env.new_string(action) {
                if let Ok(intent_obj) = env.new_object(
                    &intent_class,
                    "(Ljava/lang/String;)V",
                    &[JValue::Object(&j_action)],
                ) {
                    // addCategory(CATEGORY_OPENABLE)
                    let category = "android.intent.category.OPENABLE";
                    if let Ok(j_cat) = env.new_string(category) {
                        let _ = env.call_method(
                            &intent_obj,
                            "addCategory",
                            "(Ljava/lang/String;)Landroid/content/Intent;",
                            &[JValue::Object(&j_cat)],
                        );
                    }

                    // setType("*/*")
                    if let Ok(j_type) = env.new_string("*/*") {
                        let _ = env.call_method(
                            &intent_obj,
                            "setType",
                            "(Ljava/lang/String;)Landroid/content/Intent;",
                            &[JValue::Object(&j_type)],
                        );
                    }

                    // startActivityForResult(intent, 2001)
                    let _ = env.call_method(
                        &activity,
                        "startActivityForResult",
                        "(Landroid/content/Intent;I)V",
                        &[JValue::Object(&intent_obj), JValue::Int(2001)],
                    );
                    if env.exception_check().unwrap_or(false) {
                        let _ = env.exception_clear();
                    } else {
                        info!("System file picker intent started!");
                    }
                }
            }
        }
    }
}
