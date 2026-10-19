/*
 * Small C adapter around the UWP PasswordVault.
 *
 * Returned strings and error messages use GLib allocation and must be freed
 * with g_free().  The implementation is compiled only for UWP builds.
 */

#ifndef XEMU_UWP_PASSWORD_VAULT_H
#define XEMU_UWP_PASSWORD_VAULT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool xemu_uwp_password_vault_has(const char *resource, const char *username,
                                 char **error);
bool xemu_uwp_password_vault_store(const char *resource, const char *username,
                                   const char *password, char **error);
char *xemu_uwp_password_vault_retrieve(const char *resource,
                                       const char *username, char **error);
bool xemu_uwp_password_vault_remove(const char *resource, const char *username,
                                    char **error);

#ifdef __cplusplus
}
#endif

#endif /* XEMU_UWP_PASSWORD_VAULT_H */
