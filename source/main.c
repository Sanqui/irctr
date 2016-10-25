/*
    hi
*/

const char* DEFAULT_SERVER = "irc.";
const char* DEFAULT_NICK = "irctr";

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

#define STATE_ASK_SERVER 0
#define STATE_GET_SERVER 1
#define STATE_ASK_NICK 2
#define STATE_GET_NICK 3
#define STATE_CONNECT 4
#define STATE_ASK_CHANNEL 5
#define STATE_GET_CHANNEL 6
#define STATE_ASK_MSG 7
#define STATE_GET_MSG 8

#include <3ds.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <malloc.h>

#include "img_keyboard_bgr.h"

PrintConsole window_log, window_input;

int KEY_WIDTH = 25;
int KEY_HEIGHT = 22;
int KBD_OFF_TOP = 128;
int KBD_OFF_LEFT = 3;
int KBD_FIRST_EXTRA[] = {0, 4, 10, 14, 0};
char* KBD_CHARS[] = {"1234567890-\b\b",
                   "qwertyuiop\n\n\n",
                   "asdfghjkl'=//",
                   "zxcvbnm,.?!@"};
char* KBD_SHIFT[] = {"1234567890_\b\b",
                   "QWERTYUIOP\n\n\n",
                   "ASDFGHJKL\"*\\\\",
                   "ZXCVBNM<>:;#"};



u32 key_down;
u32 key_held;
u32 key_up;
touchPosition touch;
touchPosition last_touch;

static void *SOC_buffer = NULL;

void get_input() {
    hidScanInput();

    key_down = hidKeysDown();
    key_held = hidKeysHeld();
    key_up = hidKeysUp();
    last_touch = touch;
    hidTouchRead(&touch);
}

int sockfd;

char ds_username[256] = {};

char server_address[256] = {};
char nick[256] = {};
char channel[256] = {};
char message[256] = {};


int kbd_pos = 0;
bool kbd_done = false;
char kbd_input[256] = {};
char kbd_query[256] = {};
bool kbd_shift = false;

void kbd_setup(char* query) {
    consoleSelect(&window_input);
    consoleClear();
    kbd_done = false;
    kbd_pos = 0;
    memset(kbd_input, 0, sizeof(kbd_input));
    strcpy(kbd_query, query);
    kbd_shift = false;
}

void do_kbd() {
    if (kbd_done) {
        consoleSelect(&window_input);
        consoleClear();
        return;
    }
    if (key_up & KEY_TOUCH) {
        char key = '\0';
        if (last_touch.py >= KBD_OFF_TOP) {
            int row = (last_touch.py - KBD_OFF_TOP) / KEY_HEIGHT;
            int col = (last_touch.px - KBD_OFF_LEFT - KBD_FIRST_EXTRA[row]) / KEY_WIDTH;
            if (row == 4 && col >= 4) {
                key = ' ';
            } else if (row == 4) {
                kbd_shift = true;
            }
            else {
                if (kbd_shift)
                    key = KBD_SHIFT[row][col];
                else
                    key = KBD_CHARS[row][col];
                kbd_shift = false;
            }
        }
        if (key != '\0') {
            if (key == '\n') kbd_done = true;
            else if (key == '\b') {
                if (kbd_pos > 0) {
                    kbd_pos-=1;
                    kbd_input[kbd_pos] = '\0';
                }
            } else {
                if (kbd_pos < 255) {
                    kbd_input[kbd_pos] = key;
                    kbd_pos++;
                }
            }
        }
    }
    consoleSelect(&window_input);
    if (kbd_done) {
        consoleClear();
        return;
    }
    printf("\x1b[0;0H%s %s ", kbd_query, kbd_input);
}

void parse_irc(char* str) {
    consoleSelect(&window_log);
    char out[1024] = {};
    
    //strtok(str, ":");
    if (strstr(str, "PING") == str) {
        sprintf(out, "PONG %s\n", str+5);
        send(sockfd, out, strlen(out), 0);
        return;
    }
    str++;
    char* source = strtok(str, " ");
    
    if(strstr(source, "!")) {
        strstr(source, "!")[0] = '\0';
    }
    
    char* command = strtok(NULL, " ");
    if (!strcmp(command, "PRIVMSG")) {
        char* target = strtok(NULL, " ");
        char* rest = strtok(NULL, "\r");
        if (!strcmp(target+1, channel)) {
            printf("\x1b[1m<%s> %s\x1b[0m\n", source, rest+1);
        } else {
            printf("\x1b[1m[->%s] <%s> %s\x1b[0m\n", target, source, rest+1);
        }
    } else if (!strcmp(command, "JOIN")) {
        char* rest = strtok(NULL, "\r");
        printf("\x1b[32m > %s has joined %s\x1b[0m\n", source, rest+1);
    } else if (!strcmp(command, "QUIT")) {
        printf("\x1b[31m x %s has quit\x1b[0m\n", source);
    } else if (!strcmp(command, "PART")) {
        char* rest = strtok(NULL, "\r");
        printf("\x1b[31m < %s has parted %s\x1b[0m\n", source, rest+1);
    } else {
        char* rest = strtok(NULL, "\r");
        printf(" * %s %s %s\n", source, command, rest);
    }
}


int main(int argc, char **argv)
{
    int state, ret;
    cfguInit();
    //uint16_t tmp = CFGU_GetConfigInfoBlk2(0x1C, 0xA0000, ds_username);
    //utf16_to_utf8((uint8_t *)ds_username, &tmp, 16);
    gfxInitDefault();

    consoleInit(GFX_TOP, &window_log);
    consoleInit(GFX_TOP, &window_input);
    // 3ds is 50x30
    consoleSetWindow(&window_log, 0, 0, 50, 28);
    consoleSetWindow(&window_input, 0, 29, 50, 1);

    //consoleClear();
    consoleSelect(&window_log);
    printf("\x1b[0;0H * irctr v0.1 * \n\n");

    SOC_buffer = (void*) memalign(0x1000, 0x100000);
    if(SOC_buffer == NULL)
        return -1;
    
    
    if ((ret = socInit(SOC_buffer, SOC_BUFFERSIZE)) != 0) {
        // need to free the shared memory block if something goes wrong
        socExit();
        free(SOC_buffer);
        SOC_buffer = NULL;
        return -1;
    }

    int n = 0;
    char recv[8192];
    char out[1024] = {};
    struct sockaddr_in serv_addr; 
    memset(recv, 0, sizeof(recv));
    memset(&serv_addr, 0, sizeof(serv_addr));
    
    
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    memcpy(fb, img_keyboard_bgr, img_keyboard_bgr_size);
    
    state = STATE_ASK_SERVER;
    bool connected = false;
    
    // Main loop
    while (aptMainLoop())
    {
        if (connected) {
            memset(recv, 0, sizeof(recv));
            n = read(sockfd, recv, 8192);
            if (n < 0 && errno == EAGAIN) {
            
            } else {
                //parse_irc(recv);
                char* lines = recv;
                consoleSelect(&window_log);
                //printf("Recv: %s", command);
                while (true) {
                    char* line = lines;
                    lines = strpbrk(lines, "\n");
                    line[strcspn(line, "\n")] = '\0';
                    if (line[0] == '\0') break;
                    //printf("### Parsing: %s\n", line);
                    parse_irc(line);
                    if (!lines) break;
                    lines++;
                }
                //printf(recv);
            }
        }
        
        get_input();
        if (key_down & KEY_START) break;

        do_kbd();
        
        if (state == STATE_ASK_SERVER) {
            kbd_setup("Server address:");
            state = STATE_GET_SERVER;
            strcpy(kbd_input, DEFAULT_SERVER);
            kbd_pos = strlen(kbd_input);
        } else if (state == STATE_GET_SERVER) {
            if (kbd_done) {
                strcpy(server_address, kbd_input);
                consoleSelect(&window_log);
                printf("Server address: %s\n", server_address);
                if (!gethostbyname(server_address)) {
                    printf("Resolving address failed\n");
                    state = STATE_ASK_SERVER;
                } else {
                    state = STATE_ASK_NICK;
                }
            }
        } else if (state == STATE_ASK_NICK) {
            kbd_setup("Nick:");
            strcpy(kbd_input, DEFAULT_NICK);
            kbd_pos = strlen(kbd_input);
            state = STATE_GET_NICK;
        } else if (state == STATE_GET_NICK) {
            if (kbd_done) {
                strcpy(nick, kbd_input);
                consoleSelect(&window_log);
                printf("Nick: %s\n", nick);
                state = STATE_CONNECT;
            }
        } else if (state == STATE_CONNECT) {
            consoleSelect(&window_log);

            sockfd = socket(AF_INET, SOCK_STREAM, 0);
            if (sockfd < 0) {
                printf("socket() failed: %d\n", errno);
                state = STATE_ASK_SERVER;
                continue;
            }
            
            serv_addr.sin_family = AF_INET;
            serv_addr.sin_port = htons(6667);
            struct hostent* host_info;
            host_info = gethostbyname(server_address);
            if (!host_info) {
                printf("gethostbyname() failed\n");
                state = STATE_ASK_SERVER;
                continue;
            }
            memcpy(&serv_addr.sin_addr, host_info->h_addr_list[0], host_info->h_length);
            
            int result = connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
            if(result < 0) {
                printf("connect() failed: %d\n", errno);
                state = STATE_ASK_SERVER;
                continue;
            }
            
            printf("connected\n");
            
            sprintf(out, "NICK %s\r\n", nick);
            send(sockfd, out, strlen(out), 0);
            sprintf(out, "USER %s irctr irctr :irctr\r\n", nick);
            send(sockfd, out, strlen(out), 0);
            
            printf("sent NICK and USER\n");
            connected = true;
            result = fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);
            if (result < 0) {
                printf("fcntl failed: %d?", errno);
            }
            
            state = STATE_ASK_CHANNEL;
            printf("------------------\n");
        } else if (state == STATE_ASK_CHANNEL) {
            kbd_setup("Join channel: #");
            state = STATE_GET_CHANNEL;
        } else if (state == STATE_GET_CHANNEL) {
            if (kbd_done) {
                strcpy(channel, kbd_input);
                
                sprintf(out, "JOIN #%s\r\n", channel);
                send(sockfd, out, strlen(out), 0);
                
                state = STATE_ASK_MSG;
            }
        } else if (state == STATE_ASK_MSG) {
            char tmp[512];
            sprintf(tmp, "[#%s] <%s>", channel, nick);
            kbd_setup(tmp);
            state = STATE_GET_MSG;
        } else if (state == STATE_GET_MSG) {
            if (key_down & KEY_B) {
                sprintf(out, "PART #%s\r\n", channel);
                send(sockfd, out, strlen(out), 0);
                consoleSelect(&window_log);
                printf(" < Parted #%s\n", channel);
                state = STATE_ASK_CHANNEL;
            }
            if (kbd_done) {
                strcpy(message, kbd_input);
                
                sprintf(out, "PRIVMSG #%s :%s\r\n", channel, message);
                send(sockfd, out, strlen(out), 0);
                consoleSelect(&window_log);
                printf("<%s> %s\n", nick, message);
                
                state = STATE_ASK_MSG;
            }
        }
        

        gfxFlushBuffers();
        gfxSwapBuffers();

        gspWaitForVBlank();
    }

    //char* outquit = "QUIT :start pressed\r\n";
    //send(sockfd, outquit, strlen(outquit), 0);
        
    // Exit services
    gfxExit();
    cfguExit();
    
    ret = socExit();
    if(ret != 0)
        return -1;
    return 0;
}
