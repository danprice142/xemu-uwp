/*
 * UWP PasswordVault adapter.
 *
 * Keep failures deliberately generic: a credential, and especially its
 * password, must never become part of an error or diagnostic string.
 */

#include "qemu/osdep.h"

#include "ui/uwp-password-vault.h"

#include <windows.h>
#include <winrt/Windows.Security.Credentials.h>
#include <winrt/base.h>

#include <string>

namespace {

class ApartmentScope {
public:
    ApartmentScope()
    {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            owns_apartment = true;
        } catch (const winrt::hresult_error &exception) {
            if (exception.code() != RPC_E_CHANGED_MODE) {
                throw;
            }
            /* The caller already owns a differently configured apartment. */
        }
    }

    ~ApartmentScope()
    {
        if (owns_apartment) {
            winrt::uninit_apartment();
        }
    }

private:
    bool owns_apartment = false;
};

static bool valid_arguments(const char *resource, const char *username,
                            char **error)
{
    if (error != nullptr) {
        *error = nullptr;
    }
    if (resource != nullptr && resource[0] != '\0' && username != nullptr &&
        username[0] != '\0') {
        return true;
    }
    if (error != nullptr) {
        *error = g_strdup("Password vault resource and key are required");
    }
    return false;
}

static void set_failure(char **error, const char *operation,
                        winrt::hresult code)
{
    if (error != nullptr) {
        *error = g_strdup_printf("Password vault %s failed (0x%08lx)",
                                 operation,
                                 (unsigned long)(uint32_t)code.value);
    }
}

static bool is_not_found(winrt::hresult code)
{
    return code == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ||
           code == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
           code == E_BOUNDS;
}

} // namespace

extern "C" {

bool xemu_uwp_password_vault_has(const char *resource, const char *username,
                                 char **error)
{
    char *password = xemu_uwp_password_vault_retrieve(resource, username,
                                                       error);
    if (password == nullptr) {
        return false;
    }
    SecureZeroMemory(password, strlen(password));
    g_free(password);
    return true;
}

bool xemu_uwp_password_vault_store(const char *resource, const char *username,
                                   const char *password, char **error)
{
    if (!valid_arguments(resource, username, error) || password == nullptr) {
        if (password == nullptr && error != nullptr && *error == nullptr) {
            *error = g_strdup("Password vault password is required");
        }
        return false;
    }

    try {
        ApartmentScope apartment;
        winrt::Windows::Security::Credentials::PasswordVault vault;
        winrt::Windows::Security::Credentials::PasswordCredential previous{
            nullptr
        };
        bool had_previous = false;
        try {
            previous = vault.Retrieve(winrt::to_hstring(resource),
                                      winrt::to_hstring(username));
            previous.RetrievePassword();
            had_previous = true;
            vault.Remove(previous);
        } catch (const winrt::hresult_error &exception) {
            if (!is_not_found(exception.code())) {
                throw;
            }
        }
        winrt::Windows::Security::Credentials::PasswordCredential credential(
            winrt::to_hstring(resource), winrt::to_hstring(username),
            winrt::to_hstring(password));
        try {
            vault.Add(credential);
        } catch (...) {
            if (had_previous) {
                try {
                    vault.Add(previous);
                } catch (...) {
                    /* Preserve the original store failure reported below. */
                }
            }
            throw;
        }
        return true;
    } catch (const winrt::hresult_error &exception) {
        set_failure(error, "store", exception.code());
        return false;
    } catch (...) {
        if (error != nullptr) {
            *error = g_strdup("Password vault store failed");
        }
        return false;
    }
}

char *xemu_uwp_password_vault_retrieve(const char *resource,
                                       const char *username, char **error)
{
    if (!valid_arguments(resource, username, error)) {
        return nullptr;
    }

    try {
        ApartmentScope apartment;
        winrt::Windows::Security::Credentials::PasswordVault vault;
        auto credential = vault.Retrieve(winrt::to_hstring(resource),
                                         winrt::to_hstring(username));
        credential.RetrievePassword();
        std::string password = winrt::to_string(credential.Password());
        char *result = g_strndup(password.data(), password.size());
        if (!password.empty()) {
            SecureZeroMemory(password.data(), password.size());
        }
        return result;
    } catch (const winrt::hresult_error &exception) {
        if (is_not_found(exception.code())) {
            return nullptr;
        }
        set_failure(error, "retrieve", exception.code());
        return nullptr;
    } catch (...) {
        if (error != nullptr) {
            *error = g_strdup("Password vault retrieve failed");
        }
        return nullptr;
    }
}

bool xemu_uwp_password_vault_remove(const char *resource, const char *username,
                                    char **error)
{
    if (!valid_arguments(resource, username, error)) {
        return false;
    }

    try {
        ApartmentScope apartment;
        winrt::Windows::Security::Credentials::PasswordVault vault;
        try {
            auto credential = vault.Retrieve(winrt::to_hstring(resource),
                                             winrt::to_hstring(username));
            vault.Remove(credential);
        } catch (const winrt::hresult_error &exception) {
            if (!is_not_found(exception.code())) {
                throw;
            }
        }
        return true;
    } catch (const winrt::hresult_error &exception) {
        set_failure(error, "remove", exception.code());
        return false;
    } catch (...) {
        if (error != nullptr) {
            *error = g_strdup("Password vault remove failed");
        }
        return false;
    }
}

} // extern "C"
