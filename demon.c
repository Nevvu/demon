#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <syslog.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <sys/wait.h>

#define DEFAULT_SCAN_INTERVAL 60
#define MAX_PATTERNS 32
#define MAX_PATH_LEN 1024

volatile sig_atomic_t restart_scan = 0;
volatile sig_atomic_t stop_scan = 0;
volatile sig_atomic_t child_terminated = 0;

int verbose = 0;
pid_t children[MAX_PATTERNS];
int num_patterns = 0;

// Funkcja logująca czas
void log_time()
{
    time_t now = time(NULL);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
    syslog(LOG_INFO, "Current time: %s", time_str);
}

// Sprawdza uprawnienia do pliku/katalogu
int has_access(const char *path, int is_dir)
{
    return access(path, R_OK | (is_dir ? X_OK : 0)) == 0;
}

// Rekurencyjne skanowanie katalogu
void scan_directory(const char *dir_path, const char *pattern)
{
    if (stop_scan)
        return;

    DIR *dir = opendir(dir_path);
    if (!dir)
        return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && !stop_scan && !restart_scan)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char full_path[MAX_PATH_LEN];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0)
            continue;

        int is_dir = S_ISDIR(st.st_mode);

        if (!has_access(full_path, is_dir))
        {
            if (verbose)
                syslog(LOG_INFO, "Brak dostępu: %s", full_path);
            continue;
        }

        if (strstr(entry->d_name, pattern))
        {
            log_time();
            syslog(LOG_INFO, "Znaleziono: %s (wzorzec: %s)", full_path, pattern);
        }
        else if (verbose)
        {
            syslog(LOG_INFO, "Porównanie: %s z %s", entry->d_name, pattern);
        }

        if (is_dir)
        {
            scan_directory(full_path, pattern);
        }
    }

    closedir(dir);
}

// Sygnały dla dzieci
void sigusr1_handler(int sig)
{
    restart_scan = 1;
    if (verbose)
        syslog(LOG_INFO, "SIGUSR1: restart skanowania");
}

void sigusr2_handler(int sig)
{
    stop_scan = 1;
    if (verbose)
        syslog(LOG_INFO, "SIGUSR2: przerwanie skanowania");
}

void sigchld_handler(int sig)
{
    child_terminated = 1;
}

// Funkcja skanowania potomnego
void child_process(const char *pattern)
{
    signal(SIGUSR1, sigusr1_handler);
    signal(SIGUSR2, sigusr2_handler);

    while (1)
    {
        if (verbose)
            syslog(LOG_INFO, "Potomek '%s' – rozpoczęcie skanowania", pattern);

        stop_scan = 0;
        restart_scan = 0;
        scan_directory("/", pattern);

        if (stop_scan && verbose)
            syslog(LOG_INFO, "Potomek '%s' – skanowanie przerwane", pattern);

        if (restart_scan)
        {
            if (verbose)
                syslog(LOG_INFO, "Potomek '%s' – restart skanowania", pattern);
            continue;
        }

        if (verbose)
            syslog(LOG_INFO, "Potomek '%s' – uśpienie", pattern);
        sleep(DEFAULT_SCAN_INTERVAL);
        if (verbose)
            syslog(LOG_INFO, "Potomek '%s' – wybudzenie", pattern);
    }
}

// Rozsyłanie sygnałów potomkom
void broadcast_signal(int sig)
{
    for (int i = 0; i < num_patterns; ++i)
    {
        if (children[i] > 0)
        {
            kill(children[i], sig);
        }
    }
}

int main(int argc, char *argv[])
{
    int scan_interval = DEFAULT_SCAN_INTERVAL;
    char *patterns[MAX_PATTERNS];

    if (argc < 2)
    {
        fprintf(stderr, "Użycie: %s [-v] <fragment1> [fragment2 ...] [scan_interval]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Parsowanie argumentów
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "-v") == 0)
        {
            verbose = 1;
        }
        else if (isdigit(argv[i][0]))
        {
            scan_interval = atoi(argv[i]);
        }
        else
        {
            if (num_patterns >= MAX_PATTERNS)
            {
                fprintf(stderr, "Za dużo wzorców (max %d)\n", MAX_PATTERNS);
                exit(EXIT_FAILURE);
            }
            patterns[num_patterns++] = argv[i];
        }
    }

    if (num_patterns == 0)
    {
        fprintf(stderr, "Brak wzorców do wyszukania\n");
        exit(EXIT_FAILURE);
    }

    // Demonizacja
    if (daemon(0, 0) == -1)
    {
        perror("daemon");
        exit(EXIT_FAILURE);
    }

    openlog("demon_skanujacy", LOG_PID | LOG_CONS, LOG_USER);
    syslog(LOG_INFO, "Demon uruchomiony. Tryb verbose: %s", verbose ? "TAK" : "NIE");

    // Sygnały w procesie nadzorczym
    signal(SIGUSR1, sigusr1_handler);
    signal(SIGUSR2, sigusr2_handler);
    signal(SIGCHLD, sigchld_handler);

    // Tworzenie procesów potomnych
    for (int i = 0; i < num_patterns; ++i)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            child_process(patterns[i]);
            exit(0);
        }
        else if (pid > 0)
        {
            children[i] = pid;
        }
        else
        {
            syslog(LOG_ERR, "Błąd fork()");
            exit(EXIT_FAILURE);
        }
    }

    // Proces nadzorczy
    while (1)
    {
        if (restart_scan)
        {
            syslog(LOG_INFO, "Proces nadrzędny: przekazuję SIGUSR1 dzieciom");
            broadcast_signal(SIGUSR1);
            restart_scan = 0;
        }
        if (stop_scan)
        {
            syslog(LOG_INFO, "Proces nadrzędny: przekazuję SIGUSR2 dzieciom");
            broadcast_signal(SIGUSR2);
            stop_scan = 0;
        }

        if (child_terminated)
        {
            int status;
            pid_t pid = wait(&status);
            syslog(LOG_INFO, "Proces potomny %d zakończył się (kod %d)", pid, WEXITSTATUS(status));
            child_terminated = 0;
        }

        sleep(scan_interval);
    }

    closelog();
    return 0;
}
