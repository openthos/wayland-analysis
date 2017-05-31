/* libs/cutils/socket_local_server.c
**
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License"); 
** you may not use this file except in compliance with the License. 
** You may obtain a copy of the License at 
**
**     http://www.apache.org/licenses/LICENSE-2.0 
**
** Unless required by applicable law or agreed to in writing, software 
** distributed under the License is distributed on an "AS IS" BASIS, 
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
** See the License for the specific language governing permissions and 
** limitations under the License.
*/

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>

#include "sockets.h"

#define LISTEN_BACKLOG 4
#define ANDROID_RESERVED_SOCKET_PREFIX "/dev/socket/"

/* Documented in header file. */
static int socket_make_sockaddr_un(const char *name, int namespaceId, 
        struct sockaddr_un *p_addr, socklen_t *alen)
{
    memset (p_addr, 0, sizeof (*p_addr));
    size_t namelen;

    switch (namespaceId) {
        case ANDROID_SOCKET_NAMESPACE_ABSTRACT:
            namelen  = strlen(name);

            // Test with length +1 for the *initial* '\0'.
            if ((namelen + 1) > sizeof(p_addr->sun_path)) {
                goto error;
            }

            /*
             * Note: The path in this case is *not* supposed to be
             * '\0'-terminated. ("man 7 unix" for the gory details.)
             */

            p_addr->sun_path[0] = 0;
            memcpy(p_addr->sun_path + 1, name, namelen);
        break;

        case ANDROID_SOCKET_NAMESPACE_RESERVED:
            namelen = strlen(name) + strlen(ANDROID_RESERVED_SOCKET_PREFIX);
            /* unix_path_max appears to be missing on linux */
            if (namelen > sizeof(*p_addr)
                    - offsetof(struct sockaddr_un, sun_path) - 1) {
                goto error;
            }

            strcpy(p_addr->sun_path, ANDROID_RESERVED_SOCKET_PREFIX);
            strcat(p_addr->sun_path, name);
        break;

        case ANDROID_SOCKET_NAMESPACE_FILESYSTEM:
            namelen = strlen(name);
            /* unix_path_max appears to be missing on linux */
            if (namelen > sizeof(*p_addr)
                    - offsetof(struct sockaddr_un, sun_path) - 1) {
                goto error;
            }

            strcpy(p_addr->sun_path, name);
        break;
        default:
            // invalid namespace id
            return -1;
    }

    p_addr->sun_family = AF_LOCAL;
    *alen = namelen + offsetof(struct sockaddr_un, sun_path) + 1;
    return 0;
error:
    return -1;
}

/**
 * Binds a pre-created socket(AF_LOCAL) 's' to 'name'
 * returns 's' on success, -1 on fail
 *
 * Does not call listen()
 */
static int socket_local_server_bind(int s, const char *name, int namespaceId)
{
    struct sockaddr_un addr;
    socklen_t alen;
    int n;
    int err;

    err = socket_make_sockaddr_un(name, namespaceId, &addr, &alen);

    if (err < 0) {
        return -1;
    }

    /* basically: if this is a filesystem path, unlink first */
    unlink(addr.sun_path); /*ignore ENOENT*/

    n = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &n, sizeof(n));

    if(bind(s, (struct sockaddr *) &addr, alen) < 0) {
        return -1;
    }

    if (chmod(name, 0777) < 0) {
        weston_log("Failed to chmod file %s\n", name);
        return;
    }

    return s;

}


/** Open a server-side UNIX domain datagram socket in the Linux non-filesystem 
 *  namespace
 *
 *  Returns fd on success, -1 on fail
 */

int socket_local_server(const char *name, int namespace, int type)
{
    int err;
    int s;

    s = socket(AF_LOCAL, type, 0);
    if (s < 0) return -1;

    err = socket_local_server_bind(s, name, namespace);

    if (err < 0) {
        close(s);
        return -1;
    }

    if (type == SOCK_STREAM) {
        int ret;

        ret = listen(s, LISTEN_BACKLOG);

        if (ret < 0) {
            close(s);
            return -1;
        }
    }

    return s;
}
