/* src/include/pg_config.h.  Generated from pg_config.h.in by configure.  */
/* src/include/pg_config.h.in.  Generated from configure.in by autoheader.  */

/* Define to the type of arg 1 of 'accept' */
#define ACCEPT_TYPE_ARG1 int

/* Define to the type of arg 2 of 'accept' */
#define ACCEPT_TYPE_ARG2 struct sockaddr *

/* Define to the type of arg 3 of 'accept' */
#define ACCEPT_TYPE_ARG3 socklen_t

/* Define to the return type of 'accept' */
#define ACCEPT_TYPE_RETURN int

/* Define if building universal (internal helper macro) */
/* #undef AC_APPLE_UNIVERSAL_BUILD */

/* The alignment requirement of a `double'. */
#define ALIGNOF_DOUBLE 8

/* The alignment requirement of a `int'. */
#define ALIGNOF_INT 4

/* The alignment requirement of a `long'. */
#define ALIGNOF_LONG 8

/* The alignment requirement of a `long long int'. */
/* #undef ALIGNOF_LONG_LONG_INT */

/* The alignment requirement of a `short'. */
#define ALIGNOF_SHORT 2

/* Define to the default TCP port number on which the server listens and to
   which clients will try to connect. This can be overridden at run-time, but
   it's convenient if your clients have the right default compiled in.
   (--with-pgport=PORTNUM) */
#define DEF_PGPORT 5432

/* Define to the default TCP port number as a string constant. */
#define DEF_PGPORT_STR "5432"

/* Define to 1 to enable DTrace support. (--enable-dtrace) */
/* #undef ENABLE_DTRACE */

/* Define to build with GSSAPI support. (--with-gssapi) */
#define ENABLE_GSS 1

/* Define to 1 to build client libraries as thread-safe code.
   (--enable-thread-safety) */
#define ENABLE_THREAD_SAFETY 1

/* Define to 1 if getpwuid_r() takes a 5th argument. */
#define GETPWUID_R_5ARG /**/

/* Define to 1 if gettimeofday() takes only 1 argument. */
/* #undef GETTIMEOFDAY_1ARG */

#ifdef GETTIMEOFDAY_1ARG
# define gettimeofday(a,b) gettimeofday(a)
#endif

/* Greenplum major version as a string */
#define GP_MAJORVERSION "5"

/* A string containing the Greenplum version number */
#define GP_VERSION "5.0.0 build dev"

/* Greenplum version as a number */
#define GP_VERSION_NUM 50000

/* Define to 1 if you have the <apr_getopt.h> header file. */
#define HAVE_APR_GETOPT_H 1

/* Define to 1 if you have the `atexit' function. */
#define HAVE_ATEXIT 1

/* Define to 1 if you want to use atomics. */
#define HAVE_ATOMICS 1

/* Define to 1 if you have the <atomic.h> header file. */
/* #undef HAVE_ATOMIC_H */

/* Define to 1 if you have the `cbrt' function. */
#define HAVE_CBRT 1

/* Define to 1 if you have the `class' function. */
/* #undef HAVE_CLASS */

/* Define to 1 if you have the `crypt' function. */
#define HAVE_CRYPT 1

/* Define to 1 if you have the <crypt.h> header file. */
#define HAVE_CRYPT_H 1

/* define if the compiler supports basic C++11 syntax */
#define HAVE_CXX11 1

/* Define to 1 if you have the declaration of `CURLOPT_MAIL_FROM', and to 0 if
   you don't. */
#define HAVE_DECL_CURLOPT_MAIL_FROM 1

/* Define to 1 if you have the declaration of `fdatasync', and to 0 if you
   don't. */
#define HAVE_DECL_FDATASYNC 1

/* Define to 1 if you have the declaration of `F_FULLFSYNC', and to 0 if you
   don't. */
#define HAVE_DECL_F_FULLFSYNC 0

/* Define to 1 if you have the declaration of `posix_fadvise', and to 0 if you
   don't. */
#define HAVE_DECL_POSIX_FADVISE 1

/* Define to 1 if you have the declaration of `snprintf', and to 0 if you
   don't. */
#define HAVE_DECL_SNPRINTF 1

/* Define to 1 if you have the declaration of `strlcat', and to 0 if you
   don't. */
#define HAVE_DECL_STRLCAT 0

/* Define to 1 if you have the declaration of `strlcpy', and to 0 if you
   don't. */
#define HAVE_DECL_STRLCPY 0

/* Define to 1 if you have the declaration of `sys_siglist', and to 0 if you
   don't. */
#define HAVE_DECL_SYS_SIGLIST 1

/* Define to 1 if you have the declaration of `vsnprintf', and to 0 if you
   don't. */
#define HAVE_DECL_VSNPRINTF 1

/* Define to 1 if you have the <dld.h> header file. */
/* #undef HAVE_DLD_H */

/* Define to 1 if you have the `dlopen' function. */
#define HAVE_DLOPEN 1

/* Define to 1 if you have the <editline/history.h> header file. */
/* #undef HAVE_EDITLINE_HISTORY_H */

/* Define to 1 if you have the <editline/readline.h> header file. */
/* #undef HAVE_EDITLINE_READLINE_H */

/* Define to 1 if you have the <endian.h> header file. */
#define HAVE_ENDIAN_H 1

/* Define to 1 if you have the `ERR_set_mark' function. */
#define HAVE_ERR_SET_MARK 1

/* Define to 1 if you have the <event.h> header file. */
#define HAVE_EVENT_H 1

/* Define to 1 if you have the `fcvt' function. */
#define HAVE_FCVT 1

/* Define to 1 if you have the `fdatasync' function. */
#define HAVE_FDATASYNC 1

/* Define to 1 if you have the `fpclass' function. */
/* #undef HAVE_FPCLASS */

/* Define to 1 if you have the `fp_class' function. */
/* #undef HAVE_FP_CLASS */

/* Define to 1 if you have the `fp_class_d' function. */
/* #undef HAVE_FP_CLASS_D */

/* Define to 1 if you have the <fp_class.h> header file. */
/* #undef HAVE_FP_CLASS_H */

/* Define to 1 if fseeko (and presumably ftello) exists and is declared. */
#define HAVE_FSEEKO 1

/* Define to 1 if your compiler understands __func__. */
#define HAVE_FUNCNAME__FUNC 1

/* Define to 1 if your compiler understands __FUNCTION__. */
/* #undef HAVE_FUNCNAME__FUNCTION */

/* Define to 1 if you have __sync_lock_test_and_set(int *) and friends. */
#define HAVE_GCC_INT_ATOMICS 1

/* Define to 1 if you have __atomic_compare_exchange_n(int *, int *, int). */
#define HAVE_GCC__ATOMIC_INT32_CAS 1

/* Define to 1 if you have __atomic_compare_exchange_n(int64 *, int *, int64).
   */
#define HAVE_GCC__ATOMIC_INT64_CAS 1

/* Define to 1 if you have __sync_lock_test_and_set(char *) and friends. */
#define HAVE_GCC__SYNC_CHAR_TAS 1

/* Define to 1 if you have __sync_compare_and_swap(int *, int, int). */
#define HAVE_GCC__SYNC_INT32_CAS 1

/* Define to 1 if you have __sync_lock_test_and_set(int *) and friends. */
#define HAVE_GCC__SYNC_INT32_TAS 1

/* Define to 1 if you have __sync_compare_and_swap(int64 *, int64, int64). */
#define HAVE_GCC__SYNC_INT64_CAS 1

/* Define to 1 if you have the `getaddrinfo' function. */
#define HAVE_GETADDRINFO 1

/* Define to 1 if you have the `gethostbyname_r' function. */
#define HAVE_GETHOSTBYNAME_R 1

/* Define to 1 if you have the `getifaddrs' function. */
#define HAVE_GETIFADDRS 1

/* Define to 1 if you have the `getopt' function. */
#define HAVE_GETOPT 1

/* Define to 1 if you have the <getopt.h> header file. */
#define HAVE_GETOPT_H 1

/* Define to 1 if you have the `getopt_long' function. */
#define HAVE_GETOPT_LONG 1

/* Define to 1 if you have the `getpeereid' function. */
/* #undef HAVE_GETPEEREID */

/* Define to 1 if you have the `getpeerucred' function. */
/* #undef HAVE_GETPEERUCRED */

/* Define to 1 if you have the `getpwuid_r' function. */
#define HAVE_GETPWUID_R 1

/* Define to 1 if you have the `getrlimit' function. */
#define HAVE_GETRLIMIT 1

/* Define to 1 if you have the `getrusage' function. */
#define HAVE_GETRUSAGE 1

/* Define to 1 if you have the `gettimeofday' function. */
/* #undef HAVE_GETTIMEOFDAY */

/* Define to 1 if you have the <gpdbcost/CCostModelGPDB.h> header file. */
/* #undef HAVE_GPDBCOST_CCOSTMODELGPDB_H */

/* Define to 1 if you have the <gpopt/init.h> header file. */
/* #undef HAVE_GPOPT_INIT_H */

/* Define to 1 if you have the <gpos/_api.h> header file. */
/* #undef HAVE_GPOS__API_H */

/* Define to 1 if you have the <gssapi/gssapi.h> header file. */
#define HAVE_GSSAPI_GSSAPI_H 1

/* Define to 1 if you have the <gssapi.h> header file. */
/* #undef HAVE_GSSAPI_H */

/* Define to 1 if you have the <history.h> header file. */
/* #undef HAVE_HISTORY_H */

/* Define to 1 if you have the <ieeefp.h> header file. */
/* #undef HAVE_IEEEFP_H */

/* Define to 1 if you have the <ifaddrs.h> header file. */
#define HAVE_IFADDRS_H 1

/* Define to 1 if you have the `inet_aton' function. */
#define HAVE_INET_ATON 1

/* Define to 1 if the system has the type `int64'. */
/* #undef HAVE_INT64 */

/* Define to 1 if the system has the type `int8'. */
/* #undef HAVE_INT8 */

/* Define to 1 if the system has the type `intptr_t'. */
#define HAVE_INTPTR_T 1

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

/* Define to 1 if you have the global variable 'int opterr'. */
#define HAVE_INT_OPTERR 1

/* Define to 1 if you have the global variable 'int optreset'. */
/* #undef HAVE_INT_OPTRESET */

/* Define to 1 if you have the global variable 'int timezone'. */
#define HAVE_INT_TIMEZONE /**/

/* Define to 1 if you have support for IPv6. */
#define HAVE_IPV6 1

/* Define to 1 if you have isinf(). */
#define HAVE_ISINF 1

/* Define to 1 if you have the <kernel/image.h> header file. */
/* #undef HAVE_KERNEL_IMAGE_H */

/* Define to 1 if you have the <kernel/OS.h> header file. */
/* #undef HAVE_KERNEL_OS_H */

/* Define to 1 if `e_data' is a member of `krb5_error'. */
/* #undef HAVE_KRB5_ERROR_E_DATA */

/* Define to 1 if `text.data' is a member of `krb5_error'. */
/* #undef HAVE_KRB5_ERROR_TEXT_DATA */

/* Define to 1 if you have krb5_free_unparsed_name */
/* #undef HAVE_KRB5_FREE_UNPARSED_NAME */

/* Define to 1 if `client' is a member of `krb5_ticket'. */
/* #undef HAVE_KRB5_TICKET_CLIENT */

/* Define to 1 if `enc_part2' is a member of `krb5_ticket'. */
/* #undef HAVE_KRB5_TICKET_ENC_PART2 */

/* Define to 1 if you have the <langinfo.h> header file. */
#define HAVE_LANGINFO_H 1

/* Define to 1 if you have the <ldap.h> header file. */
/* #undef HAVE_LDAP_H */

/* Define to 1 if you have the `crypto' library (-lcrypto). */
#define HAVE_LIBCRYPTO 1

/* Define to 1 if you have the `DDBoost' library (-lDDBoost). */
/* #undef HAVE_LIBDDBOOST */

/* Define to 1 if you have the `eay32' library (-leay32). */
/* #undef HAVE_LIBEAY32 */

/* Define to 1 if you have the `gpdbcost' library (-lgpdbcost). */
/* #undef HAVE_LIBGPDBCOST */

/* Define to 1 if you have the `gpopt' library (-lgpopt). */
/* #undef HAVE_LIBGPOPT */

/* Define to 1 if you have the `gpos' library (-lgpos). */
/* #undef HAVE_LIBGPOS */

/* Define to 1 if you have the `ldap' library (-lldap). */
/* #undef HAVE_LIBLDAP */

/* Define to 1 if you have the `ldap_r' library (-lldap_r). */
/* #undef HAVE_LIBLDAP_R */

/* Define to 1 if you have the `m' library (-lm). */
#define HAVE_LIBM 1

/* Define to 1 if you have the `naucrates' library (-lnaucrates). */
/* #undef HAVE_LIBNAUCRATES */

/* Define to 1 if you have the `netsnmp' library (-lnetsnmp). */
/* #undef HAVE_LIBNETSNMP */

/* Define to 1 if you have the `numa' library (-lnuma). */
/* #undef HAVE_LIBNUMA */

/* Define to 1 if you have the `pam' library (-lpam). */
/* #undef HAVE_LIBPAM */

/* Define if you have a function readline library */
#define HAVE_LIBREADLINE 1

/* Define to 1 if you have the `rt' library (-lrt). */
#define HAVE_LIBRT 1

/* Define to 1 if you have the `ssl' library (-lssl). */
#define HAVE_LIBSSL 1

/* Define to 1 if you have the `ssleay32' library (-lssleay32). */
/* #undef HAVE_LIBSSLEAY32 */

/* Define to 1 if you have the `wldap32' library (-lwldap32). */
/* #undef HAVE_LIBWLDAP32 */

/* Define to 1 if you have the `xerces-c' library (-lxerces-c). */
/* #undef HAVE_LIBXERCES_C */

/* Define to 1 if you have the `xml2' library (-lxml2). */
#define HAVE_LIBXML2 1

/* Define to 1 if you have the `xslt' library (-lxslt). */
/* #undef HAVE_LIBXSLT */

/* Define to 1 if you have the `z' library (-lz). */
#define HAVE_LIBZ 1

/* Define to 1 if constants of type 'long long int' should have the suffix LL.
   */
/* #undef HAVE_LL_CONSTANTS */

/* Define to 1 if `long int' works and is 64 bits. */
#define HAVE_LONG_INT_64 1

/* Define to 1 if the system has the type `long long int'. */
#define HAVE_LONG_LONG_INT 1

/* Define to 1 if `long long int' works and is 64 bits. */
/* #undef HAVE_LONG_LONG_INT_64 */

/* Define to 1 if you have the <mbarrier.h> header file. */
/* #undef HAVE_MBARRIER_H */

/* Define to 1 if you have the `memmove' function. */
#define HAVE_MEMMOVE 1

/* Define to 1 if you have the <memory.h> header file. */
#define HAVE_MEMORY_H 1

/* Define to 1 if you have the <naucrates/init.h> header file. */
/* #undef HAVE_NAUCRATES_INIT_H */

/* Define to 1 if you have the <netinet/in.h> header file. */
#define HAVE_NETINET_IN_H 1

/* Define to 1 if you have the <netinet/tcp.h> header file. */
#define HAVE_NETINET_TCP_H 1

/* Define to 1 if you have the <net/if.h> header file. */
#define HAVE_NET_IF_H 1

/* Define to 1 if you have the <net-snmp/net-snmp-config.h> header file. */
/* #undef HAVE_NET_SNMP_NET_SNMP_CONFIG_H */

/* Define to 1 if you have the <numa.h> header file. */
/* #undef HAVE_NUMA_H */

/* Define to 1 if you have the `on_exit' function. */
/* #undef HAVE_ON_EXIT */

/* Define to 1 if you have the <pam/pam_appl.h> header file. */
/* #undef HAVE_PAM_PAM_APPL_H */

/* Define to 1 if you have the `poll' function. */
#define HAVE_POLL 1

/* Define to 1 if you have the <poll.h> header file. */
#define HAVE_POLL_H 1

/* Define to 1 if you have the `posix_fadvise' function. */
#define HAVE_POSIX_FADVISE 1

/* Define to 1 if you have the POSIX signal interface. */
#define HAVE_POSIX_SIGNALS /**/

/* Define to 1 if you have the `pstat' function. */
/* #undef HAVE_PSTAT */

/* Define to 1 if the PS_STRINGS thing exists. */
/* #undef HAVE_PS_STRINGS */

/* Define if you have POSIX threads libraries and header files. */
/* #undef HAVE_PTHREAD */

/* Define to 1 if you have the <pwd.h> header file. */
#define HAVE_PWD_H 1

/* Define to 1 if you have the `random' function. */
#define HAVE_RANDOM 1

/* Define to 1 if you have the <readline.h> header file. */
/* #undef HAVE_READLINE_H */

/* Define to 1 if you have the <readline/history.h> header file. */
#define HAVE_READLINE_HISTORY_H 1

/* Define to 1 if you have the <readline/readline.h> header file. */
#define HAVE_READLINE_READLINE_H 1

/* Define to 1 if you have the `readlink' function. */
#define HAVE_READLINK 1

/* Define to 1 if you have the `replace_history_entry' function. */
#define HAVE_REPLACE_HISTORY_ENTRY 1

/* Define to 1 if you have the `rint' function. */
#define HAVE_RINT 1

/* Define to 1 if you have the global variable
   'rl_completion_append_character'. */
#define HAVE_RL_COMPLETION_APPEND_CHARACTER 1

/* Define to 1 if you have the `rl_completion_matches' function. */
#define HAVE_RL_COMPLETION_MATCHES 1

/* Define to 1 if you have the `rl_filename_completion_function' function. */
#define HAVE_RL_FILENAME_COMPLETION_FUNCTION 1

/* Define to 1 if you have the <security/pam_appl.h> header file. */
/* #undef HAVE_SECURITY_PAM_APPL_H */

/* Define to 1 if you have the `setproctitle' function. */
/* #undef HAVE_SETPROCTITLE */

/* Define to 1 if you have the `setsid' function. */
#define HAVE_SETSID 1

/* Define to 1 if you have the `sigprocmask' function. */
#define HAVE_SIGPROCMASK 1

/* Define to 1 if you have sigsetjmp(). */
#define HAVE_SIGSETJMP 1

/* Define to 1 if the system has the type `sig_atomic_t'. */
#define HAVE_SIG_ATOMIC_T 1

/* Define to 1 if you have the `snprintf' function. */
#define HAVE_SNPRINTF 1

/* Define to 1 if you have spinlocks. */
#define HAVE_SPINLOCKS 1

/* Define to 1 if you have the `srandom' function. */
#define HAVE_SRANDOM 1

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the `strdup' function. */
#define HAVE_STRDUP 1

/* Define to 1 if you have the `strerror' function. */
#define HAVE_STRERROR 1

/* Define to 1 if you have the `strerror_r' function. */
#define HAVE_STRERROR_R 1

/* Define to 1 if cpp supports the ANSI # stringizing operator. */
#define HAVE_STRINGIZE 1

/* Define to 1 if you have the <strings.h> header file. */
#define HAVE_STRINGS_H 1

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if you have the `strlcat' function. */
/* #undef HAVE_STRLCAT */

/* Define to 1 if you have the `strlcpy' function. */
/* #undef HAVE_STRLCPY */

/* Define to 1 if you have the `strtol' function. */
#define HAVE_STRTOL 1

/* Define to 1 if you have the `strtoll' function. */
#define HAVE_STRTOLL 1

/* Define to 1 if you have the `strtoq' function. */
/* #undef HAVE_STRTOQ */

/* Define to 1 if you have the `strtoul' function. */
#define HAVE_STRTOUL 1

/* Define to 1 if you have the `strtoull' function. */
#define HAVE_STRTOULL 1

/* Define to 1 if you have the `strtouq' function. */
/* #undef HAVE_STRTOUQ */

/* Define to 1 if the system has the type `struct addrinfo'. */
#define HAVE_STRUCT_ADDRINFO 1

/* Define to 1 if the system has the type `struct cmsgcred'. */
/* #undef HAVE_STRUCT_CMSGCRED */

/* Define to 1 if the system has the type `struct fcred'. */
/* #undef HAVE_STRUCT_FCRED */

/* Define to 1 if the system has the type `struct option'. */
#define HAVE_STRUCT_OPTION 1

/* Define to 1 if `sa_len' is a member of `struct sockaddr'. */
/* #undef HAVE_STRUCT_SOCKADDR_SA_LEN */

/* Define to 1 if the system has the type `struct sockaddr_storage'. */
#define HAVE_STRUCT_SOCKADDR_STORAGE 1

/* Define to 1 if `ss_family' is a member of `struct sockaddr_storage'. */
#define HAVE_STRUCT_SOCKADDR_STORAGE_SS_FAMILY 1

/* Define to 1 if `ss_len' is a member of `struct sockaddr_storage'. */
/* #undef HAVE_STRUCT_SOCKADDR_STORAGE_SS_LEN */

/* Define to 1 if `__ss_family' is a member of `struct sockaddr_storage'. */
/* #undef HAVE_STRUCT_SOCKADDR_STORAGE___SS_FAMILY */

/* Define to 1 if `__ss_len' is a member of `struct sockaddr_storage'. */
/* #undef HAVE_STRUCT_SOCKADDR_STORAGE___SS_LEN */

/* Define to 1 if the system has the type `struct sockaddr_un'. */
#define HAVE_STRUCT_SOCKADDR_UN 1

/* Define to 1 if the system has the type `struct sockcred'. */
/* #undef HAVE_STRUCT_SOCKCRED */

/* Define to 1 if `tm_zone' is a member of `struct tm'. */
#define HAVE_STRUCT_TM_TM_ZONE 1

/* Define to 1 if you have the <SupportDefs.h> header file. */
/* #undef HAVE_SUPPORTDEFS_H */

/* Define to 1 if you have the `symlink' function. */
#define HAVE_SYMLINK 1

/* Define to 1 if you have the `sysconf' function. */
#define HAVE_SYSCONF 1

/* Define to 1 if you have the syslog interface. */
#define HAVE_SYSLOG 1

/* Define to 1 if you have the <sys/ioctl.h> header file. */
#define HAVE_SYS_IOCTL_H 1

/* Define to 1 if you have the <sys/ipc.h> header file. */
#define HAVE_SYS_IPC_H 1

/* Define to 1 if you have the <sys/poll.h> header file. */
#define HAVE_SYS_POLL_H 1

/* Define to 1 if you have the <sys/pstat.h> header file. */
/* #undef HAVE_SYS_PSTAT_H */

/* Define to 1 if you have the <sys/resource.h> header file. */
#define HAVE_SYS_RESOURCE_H 1

/* Define to 1 if you have the <sys/select.h> header file. */
#define HAVE_SYS_SELECT_H 1

/* Define to 1 if you have the <sys/sem.h> header file. */
#define HAVE_SYS_SEM_H 1

/* Define to 1 if you have the <sys/shm.h> header file. */
#define HAVE_SYS_SHM_H 1

/* Define to 1 if you have the <sys/socket.h> header file. */
#define HAVE_SYS_SOCKET_H 1

/* Define to 1 if you have the <sys/sockio.h> header file. */
/* #undef HAVE_SYS_SOCKIO_H */

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/tas.h> header file. */
/* #undef HAVE_SYS_TAS_H */

/* Define to 1 if you have the <sys/time.h> header file. */
#define HAVE_SYS_TIME_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <sys/un.h> header file. */
#define HAVE_SYS_UN_H 1

/* Define to 1 if you have the <termios.h> header file. */
#define HAVE_TERMIOS_H 1

/* Define to 1 if you have the <time.h> header file. */
#define HAVE_TIME_H 1

/* Define to 1 if your `struct tm' has `tm_zone'. Deprecated, use
   `HAVE_STRUCT_TM_TM_ZONE' instead. */
#define HAVE_TM_ZONE 1

/* Define to 1 if you have the `towlower' function. */
#define HAVE_TOWLOWER 1

/* Define to 1 if you have the external array `tzname'. */
#define HAVE_TZNAME 1

/* Define to 1 if you have the <ucred.h> header file. */
/* #undef HAVE_UCRED_H */

/* Define to 1 if the system has the type `uint64'. */
/* #undef HAVE_UINT64 */

/* Define to 1 if the system has the type `uint8'. */
/* #undef HAVE_UINT8 */

/* Define to 1 if the system has the type `uintptr_t'. */
#define HAVE_UINTPTR_T 1

/* Define to 1 if the system has the type `union semun'. */
/* #undef HAVE_UNION_SEMUN */

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H 1

/* Define to 1 if you have unix sockets. */
#define HAVE_UNIX_SOCKETS 1

/* Define to 1 if you have the `unsetenv' function. */
#define HAVE_UNSETENV 1

/* Define to 1 if the system has the type `unsigned long long int'. */
#define HAVE_UNSIGNED_LONG_LONG_INT 1

/* Define to 1 if you have the `utime' function. */
#define HAVE_UTIME 1

/* Define to 1 if you have the `utimes' function. */
#define HAVE_UTIMES 1

/* Define to 1 if you have the <utime.h> header file. */
#define HAVE_UTIME_H 1

/* Define to 1 if you have the `vsnprintf' function. */
#define HAVE_VSNPRINTF 1

/* Define to 1 if you have the `waitpid' function. */
#define HAVE_WAITPID 1

/* Define to 1 if you have the <wchar.h> header file. */
#define HAVE_WCHAR_H 1

/* Define to 1 if you have the `wcstombs' function. */
#define HAVE_WCSTOMBS 1

/* Define to 1 if you have the <wctype.h> header file. */
#define HAVE_WCTYPE_H 1

/* Define to 1 if you have the <winldap.h> header file. */
/* #undef HAVE_WINLDAP_H */

/* Define to 1 if you have the <winsock2.h> header file. */
/* #undef HAVE_WINSOCK2_H */

/* Define to 1 if you have the <yaml.h> header file. */
#define HAVE_YAML_H 1

/* Define to 1 if your compiler understands __builtin_constant_p. */
#define HAVE__BUILTIN_CONSTANT_P 1

/* Define to 1 if your compiler understands __builtin_types_compatible_p. */
#define HAVE__BUILTIN_TYPES_COMPATIBLE_P 1

/* Define to 1 if your compiler understands __builtin_unreachable. */
#define HAVE__BUILTIN_UNREACHABLE 1

/* Define to 1 if you have __cpuid. */
/* #undef HAVE__CPUID */

/* Define to 1 if you have __get_cpuid. */
#define HAVE__GET_CPUID 1

/* Define to 1 if your compiler understands _Static_assert. */
#define HAVE__STATIC_ASSERT 1

/* Define to 1 if your compiler understands __VA_ARGS__ in macros. */
#define HAVE__VA_ARGS 1

/* Define to the appropriate snprintf format for 64-bit ints, if any. */
#define INT64_FORMAT "%ld"

/* Define to build with Kerberos 5 support. (--with-krb5) */
/* #undef KRB5 */

/* Define as the maximum alignment requirement of any C data type. */
#define MAXIMUM_ALIGNOF 8

/* Define bytes to use libc memset(). */
#define MEMSET_LOOP_LIMIT 1024

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT "support@greenplum.org"

/* Define to the full name of this package. */
#define PACKAGE_NAME "Greenplum Database"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "Greenplum Database 5.0.0"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "greenplum-database"

/* Define to the home page for this package. */
#define PACKAGE_URL ""

/* Define to the version of this package. */
#define PACKAGE_VERSION "5.0.0"

/* Define to the name of a signed 64-bit integer type. */
#define PG_INT64_TYPE long int

/* Define to the name of the default PostgreSQL service principal in Kerberos.
   (--with-krb-srvnam=NAME) */
#define PG_KRB_SRVNAM "postgres"

/* PostgreSQL major version as a string */
#define PG_MAJORVERSION "8.3"

/* Define to 1 if "static inline" works without unwanted warnings from
   compilations where static inline functions are defined but not called. */
#define PG_USE_INLINE 1

/* Postgres version Greenplum Database is based on */
#define PG_VERSION "8.3.23"

/* PostgreSQL version as a number */
#define PG_VERSION_NUM 80323

/* A string containing the version number, platform, and C compiler */
#define PG_VERSION_STR "PostgreSQL 8.3.23 (Greenplum Database 5.0.0 build dev) on x86_64-pc-linux-gnu, compiled by GCC gcc (GCC) 4.8.2, 64-bit"

/* Define to 1 to allow profiling output to be saved separately for each
   process. */
/* #undef PROFILE_PID_DIR */

/* Define to the necessary symbol if this constant uses a non-standard name on
   your system. */
/* #undef PTHREAD_CREATE_JOINABLE */

/* The size of `off_t', as computed by sizeof. */
#define SIZEOF_OFF_T 8

/* The size of `size_t', as computed by sizeof. */
#define SIZEOF_SIZE_T 8

/* The size of `unsigned long', as computed by sizeof. */
#define SIZEOF_UNSIGNED_LONG 8

/* The size of `void *', as computed by sizeof. */
#define SIZEOF_VOID_P 8

/* Define to 1 if you have the ANSI C header files. */
#define STDC_HEADERS 1

/* Define to 1 if strerror_r() returns a int. */
/* #undef STRERROR_R_INT */

/* Define to 1 if your <sys/time.h> declares `struct tm'. */
/* #undef TM_IN_SYS_TIME */

/* Define to the appropriate snprintf format for unsigned 64-bit ints, if any.
   */
#define UINT64_FORMAT "%lu"

/* Define to 1 to build with assertion checks. (--enable-cassert) */
/* #undef USE_ASSERT_CHECKING */

/* Define to 1 to build with Bonjour support. (--with-bonjour) */
/* #undef USE_BONJOUR */

/* Define to 1 to build with libcurl support. (--with-libcurl) */
#define USE_CURL 1

/* Define to 1 to build with DD Boost capabilities. (--enable-ddboost) */
/* #undef USE_DDBOOST */

/* Define to 1 to build with debug_break capabilities. (--enable-debugbreak)
   */
/* #undef USE_DEBUG_BREAK */

/* Define to 1 to build with debug_ntuplestore. (--enable-ntuplestore) */
/* #undef USE_DEBUG_NTUPLESTORE */

/* Define to 1 to build with gpcloud (--enable-gpcloud) */
#define USE_GPCLOUD 1

/* Define to 1 if you want 64-bit integer timestamp and interval support.
   (--enable-integer-datetimes) */
#define USE_INTEGER_DATETIMES 1

/* Define to 1 to build with LDAP support. (--with-ldap) */
/* #undef USE_LDAP */

/* Define to 1 to build with XML support. (--with-libxml) */
#define USE_LIBXML 1

/* Define to 1 to use XSLT support when building contrib/xml2.
   (--with-libxslt) */
/* #undef USE_LIBXSLT */

/* Define to 1 to build with Mapreduce capabilities (--enable-mapreduce) */
/* #undef USE_MAPREDUCE */

/* Define to select named POSIX semaphores. */
/* #undef USE_NAMED_POSIX_SEMAPHORES */

/* Define to 1 to build with NetBackup capabilities. (--enable-netbackup) */
/* #undef USE_NETBACKUP */

/* Define to 1 to build with Greenplum ORCA optimizer. (--enable-orca) */
/* #undef USE_ORCA */

/* Define to 1 to build with PAM support. (--with-pam) */
/* #undef USE_PAM */

/* Use replacement snprintf() functions. */
/* #undef USE_REPL_SNPRINTF */

/* Define to 1 to use Intel SSE 4.2 CRC instructions with a runtime check. */
/* #undef USE_SLICING_BY_8_CRC32C */

/* Define to 1 to build with snmp capabilities. (--enable-snmp) */
/* #undef USE_SNMP */

/* Define to 1 use Intel SSE 4.2 CRC instructions. */
/* #undef USE_SSE42_CRC32C */

/* Define to 1 to use Intel SSSE 4.2 CRC instructions with a runtime check. */
#define USE_SSE42_CRC32C_WITH_RUNTIME_CHECK 1

/* Define to build with (Open)SSL support. (--with-openssl) */
#define USE_SSL 1

/* Define to select SysV-style semaphores. */
#define USE_SYSV_SEMAPHORES 1

/* Define to select SysV-style shared memory. */
#define USE_SYSV_SHARED_MEMORY 1

/* Define to 1 to build with testing utilities. (--enable-testutils) */
/* #undef USE_TEST_UTILS */

/* Define to select unnamed POSIX semaphores. */
/* #undef USE_UNNAMED_POSIX_SEMAPHORES */

/* Define to select Win32-style semaphores. */
/* #undef USE_WIN32_SEMAPHORES */

/* Define to select Win32-style shared memory. */
/* #undef USE_WIN32_SHARED_MEMORY */

/* Define WORDS_BIGENDIAN to 1 if your processor stores words with the most
   significant byte first (like Motorola and SPARC, unlike Intel). */
#if defined AC_APPLE_UNIVERSAL_BUILD
# if defined __BIG_ENDIAN__
#  define WORDS_BIGENDIAN 1
# endif
#else
# ifndef WORDS_BIGENDIAN
/* #  undef WORDS_BIGENDIAN */
# endif
#endif

/* Enable large inode numbers on Mac OS X 10.5.  */
#ifndef _DARWIN_USE_64_BIT_INODE
# define _DARWIN_USE_64_BIT_INODE 1
#endif

/* Number of bits in a file offset, on hosts where this is settable. */
/* #undef _FILE_OFFSET_BITS */

/* Define to 1 to make fseeko visible on some hosts (e.g. glibc 2.2). */
/* #undef _LARGEFILE_SOURCE */

/* Define for large files, on AIX-style hosts. */
/* #undef _LARGE_FILES */

/* Define to empty if `const' does not conform to ANSI C. */
/* #undef const */

/* Define to `__inline__' or `__inline' if that's what the C compiler
   calls it, or to nothing if 'inline' is not supported under any name.  */
#ifndef __cplusplus
/* #undef inline */
#endif

/* Define to the type of a signed integer type wide enough to hold a pointer,
   if such a type exists, and if the system does not define it. */
/* #undef intptr_t */

/* Define to empty if the C compiler does not understand signed types. */
/* #undef signed */

/* Define to the type of an unsigned integer type wide enough to hold a
   pointer, if such a type exists, and if the system does not define it. */
/* #undef uintptr_t */

/* Define to empty if the keyword `volatile' does not work. Warning: valid
   code using `volatile' can become incorrect without. Disable with care. */
/* #undef volatile */
