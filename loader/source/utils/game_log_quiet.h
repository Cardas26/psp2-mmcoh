#ifndef SOLOADER_GAME_LOG_QUIET_H
#define SOLOADER_GAME_LOG_QUIET_H

#if defined(GAME_LOG_QUIET)
void game_log_quiet_install(void);
#else
#define game_log_quiet_install() ((void)0)
#endif

#define game_log_quiet_ab_arm()  (-1)

#endif
