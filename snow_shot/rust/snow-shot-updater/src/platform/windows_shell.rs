//! Remove or migrate only shell verbs owned by the target installation and edition.
use crate::error::{Result, UpdateError};
use path_clean::PathClean;
use std::ffi::{OsStr, OsString};
use std::os::windows::ffi::{OsStrExt, OsStringExt};
use std::path::{Path, PathBuf};
use windows::Win32::Foundation::{
    ERROR_FILE_NOT_FOUND, ERROR_MORE_DATA, ERROR_NO_MORE_ITEMS, ERROR_PATH_NOT_FOUND,
    ERROR_SUCCESS, ERROR_UNSUPPORTED_TYPE, WIN32_ERROR,
};
use windows::Win32::Globalization::{CSTR_EQUAL, CompareStringOrdinal};
use windows::Win32::System::Registry::{
    HKEY, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, HKEY_USERS, KEY_READ, KEY_WRITE,
    REG_PROCESS_APPKEY, REG_SAM_FLAGS, REG_SZ, RRF_RT_REG_EXPAND_SZ, RRF_RT_REG_SZ, RegCloseKey,
    RegDeleteTreeW, RegEnumKeyExW, RegFlushKey, RegGetValueW, RegLoadAppKeyW, RegOpenKeyExW,
    RegSetValueExW,
};
use windows::core::{PCWSTR, PWSTR};

const FAILURE_CODE: &str = "shell_registration_failed";
const FAILURE_MESSAGE: &str = "Could not update Snow Shot context menu registration";
const ASSOCIATIONS: &str = "SystemFileAssociations";
const CLASSES: &str = "Software\\Classes";
const PROFILES: &str = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList";

struct Key(HKEY);
impl Drop for Key {
    fn drop(&mut self) {
        unsafe {
            let _ = RegCloseKey(self.0);
        }
    }
}

fn wide(value: impl AsRef<OsStr>) -> Vec<u16> {
    value.as_ref().encode_wide().chain(Some(0)).collect()
}

fn error(operation: &str, status: WIN32_ERROR) -> UpdateError {
    UpdateError::new(FAILURE_CODE, FAILURE_MESSAGE)
        .detail(format!("{operation}: Win32 error {}", status.0))
}

fn check(operation: &str, status: WIN32_ERROR) -> Result<()> {
    if status == ERROR_SUCCESS {
        Ok(())
    } else {
        Err(error(operation, status))
    }
}

fn open(base: HKEY, path: impl AsRef<OsStr>, access: REG_SAM_FLAGS) -> Result<Option<Key>> {
    let path = wide(path);
    let mut key = HKEY::default();
    let status = unsafe { RegOpenKeyExW(base, PCWSTR(path.as_ptr()), None, access, &mut key) };
    if matches!(status, ERROR_FILE_NOT_FOUND | ERROR_PATH_NOT_FOUND) {
        return Ok(None);
    }
    check("RegOpenKeyExW", status)?;
    Ok(Some(Key(key)))
}

fn read(key: HKEY, name: &str, expandable: bool) -> Result<Option<OsString>> {
    let name = wide(name);
    let mut value = vec![0_u16; 32_768];
    let mut bytes = (value.len() * size_of::<u16>()) as u32;
    let flags = if expandable {
        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ
    } else {
        RRF_RT_REG_SZ
    };
    let status = unsafe {
        RegGetValueW(
            key,
            PCWSTR::null(),
            PCWSTR(name.as_ptr()),
            flags,
            None,
            Some(value.as_mut_ptr().cast()),
            Some(&mut bytes),
        )
    };
    if matches!(
        status,
        ERROR_FILE_NOT_FOUND | ERROR_UNSUPPORTED_TYPE | ERROR_MORE_DATA
    ) {
        return Ok(None);
    }
    check("RegGetValueW", status)?;
    let length = bytes as usize / size_of::<u16>();
    if bytes < 2
        || bytes as usize > size_of_val(&*value)
        || !bytes.is_multiple_of(2)
        || value[length - 1] != 0
        || value[..length - 1].contains(&0)
    {
        return Ok(None);
    }
    Ok(Some(OsString::from_wide(&value[..length - 1])))
}

fn write(key: HKEY, name: &str, value: impl AsRef<OsStr>) -> Result<()> {
    let name = wide(name);
    let value = wide(value);
    let bytes = unsafe {
        std::slice::from_raw_parts(value.as_ptr().cast::<u8>(), value.len() * size_of::<u16>())
    };
    check("RegSetValueExW", unsafe {
        RegSetValueExW(key, PCWSTR(name.as_ptr()), None, REG_SZ, Some(bytes))
    })
}

fn names(key: HKEY) -> Result<Vec<OsString>> {
    let mut result = Vec::new();
    for index in 0.. {
        let mut name = vec![0_u16; 256];
        let mut length = name.len() as u32;
        let status = unsafe {
            RegEnumKeyExW(
                key,
                index,
                Some(PWSTR(name.as_mut_ptr())),
                &mut length,
                None,
                None,
                None,
                None,
            )
        };
        if status == ERROR_NO_MORE_ITEMS {
            break;
        }
        check("RegEnumKeyExW", status)?;
        result.push(OsString::from_wide(&name[..length as usize]));
    }
    Ok(result)
}

fn equal(left: impl AsRef<OsStr>, right: impl AsRef<OsStr>) -> bool {
    let left = left.as_ref().encode_wide().collect::<Vec<_>>();
    let right = right.as_ref().encode_wide().collect::<Vec<_>>();
    unsafe { CompareStringOrdinal(&left, &right, true) == CSTR_EQUAL }
}

fn normalized(path: &Path) -> String {
    let native = path.clean().to_string_lossy().replace('/', "\\");
    if let Some(unc) = native.strip_prefix("\\\\?\\UNC\\") {
        format!("\\\\{unc}")
    } else {
        native.strip_prefix("\\\\?\\").unwrap_or(&native).to_owned()
    }
}

fn same_executable(value: &OsStr, expected: &Path) -> bool {
    Path::new(value).is_absolute() && equal(normalized(Path::new(value)), normalized(expected))
}

fn owned(key: HKEY, executable: &Path) -> Result<bool> {
    let Some(product) = read(key, "ProductId", false)? else {
        return Ok(false);
    };
    let Some(owner) = read(key, "InstallationExecutable", false)? else {
        return Ok(false);
    };
    if product != crate::edition::PRODUCT || !same_executable(&owner, executable) {
        return Ok(false);
    }
    let Some(command) = open(key, "command", KEY_READ)? else {
        return Ok(false);
    };
    let Some(value) = read(command.0, "", false)? else {
        return Ok(false);
    };
    let Some(value) = value.to_str() else {
        return Ok(false);
    };
    let Some((target, arguments)) = value
        .strip_prefix('"')
        .and_then(|value| value.split_once('"'))
    else {
        return Ok(false);
    };
    Ok(arguments == " --edit-with-snow-shot \"%1\""
        && same_executable(OsStr::new(target), executable))
}

fn update_classes(classes: HKEY, executable: &Path, replacement: Option<&Path>) -> Result<bool> {
    let Some(associations) = open(classes, ASSOCIATIONS, KEY_READ)? else {
        return Ok(false);
    };
    let mut changed = false;
    let mut failure = None;
    for extension in names(associations.0)? {
        if !extension.to_string_lossy().starts_with('.') {
            continue;
        }
        let relative = PathBuf::from(extension)
            .join("shell")
            .join(crate::edition::SHELL_VERB);
        let operation = (|| -> Result<bool> {
            let Some(verb) = open(associations.0, relative.as_os_str(), KEY_READ)? else {
                return Ok(false);
            };
            if !owned(verb.0, executable)? {
                return Ok(false);
            }
            drop(verb);
            if let Some(replacement) = replacement {
                let Some(verb) = open(associations.0, relative.as_os_str(), KEY_READ | KEY_WRITE)?
                else {
                    return Ok(false);
                };
                // Recheck after acquiring write access; another installation can claim the verb.
                if !owned(verb.0, executable)? {
                    return Ok(false);
                }
                let replacement = normalized(replacement);
                let Some(command) = open(verb.0, "command", KEY_READ | KEY_WRITE)? else {
                    return Ok(false);
                };
                write(
                    command.0,
                    "",
                    format!("\"{replacement}\" --edit-with-snow-shot \"%1\""),
                )?;
                write(verb.0, "Icon", format!("\"{replacement}\",0"))?;
                write(verb.0, "InstallationExecutable", replacement)?;
            } else {
                let Some(associations) = open(classes, ASSOCIATIONS, KEY_READ | KEY_WRITE)? else {
                    return Ok(false);
                };
                let Some(verb) = open(associations.0, relative.as_os_str(), KEY_READ)? else {
                    return Ok(false);
                };
                if !owned(verb.0, executable)? {
                    return Ok(false);
                }
                drop(verb);
                let relative = wide(relative.as_os_str());
                check("RegDeleteTreeW", unsafe {
                    RegDeleteTreeW(associations.0, PCWSTR(relative.as_ptr()))
                })?;
            }
            Ok(true)
        })();
        match operation {
            Ok(updated) => changed |= updated,
            Err(error) => {
                failure.get_or_insert(error);
            }
        }
    }
    failure.map_or(Ok(changed), Err)
}

fn user_sid(sid: &str) -> bool {
    sid.starts_with("S-1-5-21-") || sid.starts_with("S-1-12-1-")
}

fn update_installation_shell_verbs(root: &Path, replacement: Option<&Path>) -> Result<()> {
    let executable = root.join(crate::edition::APP_PATH);
    let replacement = replacement.map(|root| root.join(crate::edition::APP_PATH));
    let replacement = replacement.as_deref();
    let mut failure = None;
    let mut record = |result: Result<()>| {
        if let Err(error) = result {
            failure.get_or_insert(error);
        }
    };
    record((|| {
        if let Some(classes) = open(HKEY_CURRENT_USER, CLASSES, KEY_READ)? {
            update_classes(classes.0, &executable, replacement)?;
        }
        Ok(())
    })());
    let mut loaded = std::collections::HashSet::new();
    let users = match names(HKEY_USERS) {
        Ok(users) => users,
        Err(error) => {
            record(Err(error));
            Vec::new()
        }
    };
    for name in users {
        let text = name.to_string_lossy();
        let Some(sid) = text.strip_suffix("_Classes").filter(|sid| user_sid(sid)) else {
            continue;
        };
        loaded.insert(sid.to_owned());
        record((|| {
            if let Some(classes) = open(HKEY_USERS, &name, KEY_READ)? {
                update_classes(classes.0, &executable, replacement)?;
            }
            Ok(())
        })());
    }
    let profiles = match open(HKEY_LOCAL_MACHINE, PROFILES, KEY_READ) {
        Ok(profiles) => profiles,
        Err(error) => {
            record(Err(error));
            None
        }
    };
    if let Some(profiles) = profiles {
        let names = match names(profiles.0) {
            Ok(names) => names,
            Err(error) => {
                record(Err(error));
                Vec::new()
            }
        };
        for sid in names {
            let text = sid.to_string_lossy();
            if !user_sid(&text) || loaded.contains(text.as_ref()) {
                continue;
            }
            record((|| {
                let Some(profile) = open(profiles.0, &sid, KEY_READ)? else {
                    return Ok(());
                };
                let Some(directory) = read(profile.0, "ProfileImagePath", true)? else {
                    return Ok(());
                };
                let hive =
                    PathBuf::from(directory).join("AppData/Local/Microsoft/Windows/UsrClass.dat");
                if !hive.is_file() {
                    return Ok(());
                }
                let hive_path = wide(hive.as_os_str());
                let mut hive = HKEY::default();
                check("RegLoadAppKeyW", unsafe {
                    RegLoadAppKeyW(
                        PCWSTR(hive_path.as_ptr()),
                        &mut hive,
                        (KEY_READ | KEY_WRITE).0,
                        REG_PROCESS_APPKEY,
                        None,
                    )
                })?;
                let hive = Key(hive);
                if update_classes(hive.0, &executable, replacement)? {
                    check("RegFlushKey", unsafe { RegFlushKey(hive.0) })?;
                }
                Ok(())
            })());
        }
    }
    failure.map_or(Ok(()), Err)
}

pub(super) fn remove_installation_shell_verbs(root: &Path) -> Result<()> {
    update_installation_shell_verbs(root, None)
}

pub(super) fn migrate_installation_shell_verbs(previous: &Path, root: &Path) -> Result<()> {
    if equal(normalized(previous), normalized(root)) {
        return Ok(());
    }
    update_installation_shell_verbs(previous, Some(root))
}
