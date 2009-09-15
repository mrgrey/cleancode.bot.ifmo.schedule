/*
 * File:   main.cpp
 * Author: mrgrey, thorn (cleancode)
 *
 * Created on 1 Сентябрь 2009 г., 0:02
 */

#include <stdlib.h>
#include <glib-2.0/glib.h>
#include <libpurple/purple.h>
#include <curl/curl.h>
#include <iconv.h>
#include <getopt.h>

#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

// <editor-fold defaultstate="collapsed" desc="define section">
#define CUSTOM_USER_DIRECTORY  "/dev/null"
#define CUSTOM_PLUGIN_PATH     ""
#define PLUGIN_SAVE_PREF       "/purple/nullclient/plugins/saved"
#define UI_ID                  "cleancode.bot.ifmo.schedule"


#define PURPLE_GLIB_READ_COND  (G_IO_IN | G_IO_HUP | G_IO_ERR)
#define PURPLE_GLIB_WRITE_COND (G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL)
// </editor-fold>

#define COMMANDS_TABLE_INIT() commands_table = g_hash_table_new(g_str_hash, g_str_equal);
#define COMMANDS_TABLE_ENTRY(cmd,func) g_hash_table_insert(commands_table, (void*)cmd, (void*)func);

#define STATUS_AVAILABLE 0
#define STATUS_OFFLINE 1
#define STATUS_AWAY 2
#define STATUS_MAX_ID STATUS_AWAY

#define INIT_STATUS(our_code, purple_code) 	status_types[our_code] = purple_status_type_new(purple_code, NULL, NULL, true);\
								statuses[our_code] = purple_status_new(status_types[our_code], presence);
#define STATUS_ID(our_code) purple_status_get_id(statuses[our_code])

char uin[10];
char password[30];

int requests_schedule_count = 0;

static PurplePlugin *icqPlugin;
static PurplePluginInfo *icqPluginInfo;
static PurplePresence *presence;
static PurpleStatusType* status_types[STATUS_MAX_ID + 1];
static PurpleStatus* statuses[STATUS_MAX_ID + 1];

static PurpleAccount *globalAccount;

#define LOG_CATEGORY_GENERAL 0x1
#define LOG_CATEGORY_NONE 0
#define LOG_CATEGORY_ALL 0xFFFFFFFF

#define LOG_BUFFER_LENGTH 200

static unsigned int LogCategories = LOG_CATEGORY_ALL;

static FILE* LogFile = NULL;

bool log_init(const char* logFileName){
	if(LogFile)
		return false;
	LogFile = fopen(logFileName, "a");
}

void log_out(unsigned int category, const char *message){
	if(!(category & LogCategories))
		return;
	
	char date[24];
	const time_t now = time(NULL);
	strcpy(date, purple_utf8_strftime("[%d.%m.%Y %H:%M:%S] ", localtime(&now)));
	
	if(LogFile){
		fwrite(date, 1, strlen(date),LogFile);
		fwrite(message, 1, strlen(message), LogFile);
		if(message[strlen(message)-1] != '\n')
			fwrite("\n", 1, 1, LogFile);
		fflush(LogFile);
	}
	else{
		printf("%s%s", date, message);
		if(message[strlen(message)-1] != '\n')
			printf("\n");
	}
}

void log_uninit(){
	if(LogFile)
		return;
	fclose(LogFile);
}

void wait(int seconds){
	clock_t endwait;
	endwait=clock()+seconds*CLOCKS_PER_SEC;
	while (clock()<endwait);
}

int min(int a, int b){
	return a<b?a:b;
}


static char day_of_week[][23]={
	"Воскресенье",
    "Понедельник",
    "Вторник",
    "Среда",
    "Четверг",
    "Пятница",
    "Суббота",
    "Воскресенье"
};

typedef struct _PurpleGLibIOClosure {
    PurpleInputFunction function;
    guint result;
    gpointer data;
} PurpleGLibIOClosure;

static void purple_glib_io_destroy(gpointer data) {
    g_free(data);
}

static gboolean purple_glib_io_invoke(GIOChannel *source, GIOCondition condition, gpointer data) {
    PurpleGLibIOClosure *closure = (PurpleGLibIOClosure *) data;
    PurpleInputCondition purple_cond = (PurpleInputCondition) 0;

    if (condition & PURPLE_GLIB_READ_COND)
        purple_cond = (PurpleInputCondition) (purple_cond | PURPLE_INPUT_READ);
    if (condition & PURPLE_GLIB_WRITE_COND)
        purple_cond = (PurpleInputCondition) (purple_cond | PURPLE_INPUT_WRITE);

    closure->function(closure->data, g_io_channel_unix_get_fd(source),
            purple_cond);

    return TRUE;
}

static guint glib_input_add(gint fd, PurpleInputCondition condition, PurpleInputFunction function,
        gpointer data) {
    PurpleGLibIOClosure *closure = g_new0(PurpleGLibIOClosure, 1);
    GIOChannel *channel;
    GIOCondition cond = (GIOCondition) 0;

    closure->function = function;
    closure->data = data;

    if (condition & PURPLE_INPUT_READ)
        cond = (GIOCondition) (cond | PURPLE_GLIB_READ_COND);
    if (condition & PURPLE_INPUT_WRITE)
        cond = (GIOCondition) (cond | PURPLE_GLIB_WRITE_COND);

    channel = g_io_channel_unix_new(fd);
    closure->result = g_io_add_watch_full(channel, G_PRIORITY_DEFAULT, cond,
            purple_glib_io_invoke, closure, purple_glib_io_destroy);

    g_io_channel_unref(channel);
    return closure->result;
}

static PurpleEventLoopUiOps glib_eventloops ={
    g_timeout_add,
    g_source_remove,
    glib_input_add,
    g_source_remove,
    NULL,
#if GLIB_CHECK_VERSION(2,14,0)
    g_timeout_add_seconds,
#else
    NULL,
#endif

    /* padding */
    NULL,
    NULL,
    NULL
};
/*** End of the eventloop functions. ***/

static char * get_answer(const char *command, char *answer);

/*** Conversation uiops ***/
// <editor-fold defaultstate="collapsed" desc="conversation uiops">

static void write_conv(PurpleConversation *conv, const char *who, const char *alias,
        const char *message, PurpleMessageFlags flags, time_t mtime) {
    if (!(flags & PURPLE_MESSAGE_RECV))
        return;

    const char *name;
    if (alias && *alias)
        name = alias;
    else if (who && *who)
        name = who;
    else
        name = NULL;

	const char* conversation_name = purple_conversation_get_name(conv);
		
    /*printf("(%s) %s %s: %s\n", conversation_name,
            purple_utf8_strftime("(%H:%M:%S)", localtime(&mtime)),
            name, message);
	*/	
	
	//бредокод, работает
	char log_buffer[LOG_BUFFER_LENGTH];
	strcpy(log_buffer, conversation_name);
	strcat(log_buffer, ": \""); //3

	int max_message_to_copy = LOG_BUFFER_LENGTH - strlen(conversation_name) - 5;
	
	int length_to_copy = max_message_to_copy <= strlen(message) ? max_message_to_copy : strlen(message);
	
	memcpy(log_buffer + strlen(conversation_name) + 3, message, length_to_copy);
	
	log_buffer[strlen(conversation_name) + 3 + length_to_copy] = 0;
	
	strcat(log_buffer, "\""); //1
	log_buffer[strlen(log_buffer)] = 0;
	
	log_out(LOG_CATEGORY_GENERAL, log_buffer);

    char send_message[5120];
    get_answer(message, &send_message[0]);
    
    purple_conv_im_send(PURPLE_CONV_IM(conv), send_message);
}

static PurpleConversationUiOps conv_uiops = {
    NULL, /* create_conversation  */
    NULL, /* destroy_conversation */
    NULL, /* write_chat           */
    NULL, /* write_im             */
    write_conv, /* write_conv           */
    NULL, /* chat_add_users       */
    NULL, /* chat_rename_user     */
    NULL, /* chat_remove_users    */
    NULL, /* chat_update_user     */
    NULL, /* present              */
    NULL, /* has_focus            */
    NULL, /* custom_smiley_add    */
    NULL, /* custom_smiley_write  */
    NULL, /* custom_smiley_close  */
    NULL, /* send_confirm         */
    NULL,
    NULL,
    NULL,
    NULL
};

static void ui_init(void) {
    purple_conversations_set_ui_ops(&conv_uiops);
}// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="libpurple init">
static PurpleCoreUiOps core_uiops = {
    NULL,
    NULL,
    ui_init,
    NULL,

    /* padding */
    NULL,
    NULL,
    NULL,
    NULL
};

static void init_libpurple(void) {
    /* Set a custom user directory (optional) */
    purple_util_set_user_dir(CUSTOM_USER_DIRECTORY);

    /* We do not want any debugging for now to keep the noise to a minimum. */
    purple_debug_set_enabled(FALSE);

    /* Set the core-uiops, which is used to
     * 	- initialize the ui specific preferences.
     * 	- initialize the debug ui.
     * 	- initialize the ui components for all the modules.
     * 	- uninitialize the ui components for all the modules when the core terminates.
     */
    purple_core_set_ui_ops(&core_uiops);

    /* Set the uiops for the eventloop. If your client is glib-based, you can safely
     * copy this verbatim. */
    purple_eventloop_set_ui_ops(&glib_eventloops);

    /* Set path to search for plugins. The core (libpurple) takes care of loading the
     * core-plugins, which includes the protocol-plugins. So it is not essential to add
     * any path here, but it might be desired, especially for ui-specific plugins. */
    purple_plugins_add_search_path(CUSTOM_PLUGIN_PATH);

    /* Now that all the essential stuff has been set, let's try to init the core. It's
     * necessary to provide a non-NULL name for the current ui to the core. This name
     * is used by stuff that depends on this ui, for example the ui-specific plugins. */
    if (!purple_core_init(UI_ID)) {
        /* Initializing the core failed. Terminate. */
        fprintf(stderr,
                "libpurple initialization failed. Dumping core.\n"
                "Please report this!\n");
        abort();
    }

    /* Create and load the buddylist. */
    purple_set_blist(purple_blist_new());
    purple_blist_load();

    /* Load the preferences. */
    purple_prefs_load();

    /* Load the desired plugins. The client should save the list of loaded plugins in
     * the preferences using purple_plugins_save_loaded(PLUGIN_SAVE_PREF) */
    purple_plugins_load_saved(PLUGIN_SAVE_PREF);

    /* Load the pounces. */
    purple_pounces_load();
}// </editor-fold>

static void signed_on(PurpleConnection *gc, gpointer null) {
    PurpleAccount *account = purple_connection_get_account(gc);
    printf("Account connected: %s %s\n", account->username, account->protocol_id);
}

static void signed_off(PurpleConnection *gc, gpointer null) {
    PurpleAccount *account = purple_connection_get_account(gc);
    printf("Account disconnected: %s %s\n", account->username, account->protocol_id);
}

static void connect_to_signals(void) {
    static int handle;
    purple_signal_connect(purple_connections_get_handle(), "signed-on", &handle,
            PURPLE_CALLBACK(signed_on), NULL);
     purple_signal_connect(purple_connections_get_handle(), "signed-off", &handle,
            PURPLE_CALLBACK(signed_off), NULL);

}

static size_t write_data(char *buffer, size_t size, size_t nmemb, char *userp) {
    strcpy(userp, buffer);
    return size*nmemb;
}

static char * get_schedule_json(int group, char *buffer, time_t date = 0) {
    CURL *curl;
    CURLcode res;
	char dateStr[11];
	struct tm *dateInfo;

    curl = curl_easy_init();
    if (curl) {
		if(!date){
			date = time(NULL);
		}
		dateInfo = localtime(&date);
		strftime(dateStr, 11, "%d.%m.%Y", dateInfo);
        char url[131]; //101
        sprintf(&url[0], "http://faculty.ifmo.ru/gadgets/spbsuitmo-schedule-lessons/data/lessons-proxy-json.php?gr=%d&date=%s", group, dateStr);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, buffer);
        res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }

    return buffer;
}

static char * convert_from_utf(const guint16 utf_symbol, char *converted_data);

static char * decode_utf_literals(const char *json_data, char *buffer) {
    char *startptr = buffer;
    char tdata;
    guint16 tvalue;
    char *curptr = (char *)json_data;
    while(*curptr != '\0') {
        if(*curptr == '\\' && *(curptr+1) == 'u') {
            tdata = *(curptr+6);
            *(curptr+6) = '\\';
            tvalue = (guint16)strtol(curptr+2,NULL,16);
            convert_from_utf(tvalue, buffer);
            buffer += 2;
            *(curptr+6) = tdata;
            curptr += 6;
        } else {
            *(buffer++) = *(curptr++);
        }
        *buffer = 0;
    }    return startptr;
}

typedef struct{
    char time[30];
    char place[30];
    char subject[5120];
    char person_name[100];
} pair;

typedef struct {
    int week_number;
    char day[23];
    int group;
    pair lessons[8];
} data;

static int parse_json(const char *json_data, data *data) {
    char *startptr, *endptr, statusSymbol;
    startptr = strstr(json_data, "status");
	statusSymbol = startptr[9];
	if(statusSymbol == 'w')
	{
		return -1;
	}
	
	
    startptr = strstr(json_data, "week_number");
    data->week_number = (int)strtol(startptr+13,NULL,10);
    startptr = strstr(startptr, "day");
    strcpy(&(data->day[0]) ,&day_of_week[(int)strtol(startptr+6,NULL,10)][0]);
    startptr = strstr(startptr, "group");
    data->group = (int)strtol(startptr+8,NULL,10);
	
	if(statusSymbol == 'n')
	{
		return 0;
	}

    for(int i = 0; i < 8; i++) {
        //time
        startptr = strstr(startptr, "time");
        if( startptr == NULL ) {
            return i+1;
        }
        endptr = strstr(startptr, "place");
        memcpy(data->lessons[i].time, startptr+7, endptr-startptr-10);
        data->lessons[i].time[endptr-startptr-10] = 0;
        //place
        startptr = endptr;
        endptr = strstr(startptr, "subject");
        memcpy(data->lessons[i].place, startptr+8, endptr-startptr-11);
        data->lessons[i].place[endptr-startptr-11] = 0;
        //subject
        startptr = endptr;
        endptr = strstr(startptr, "person_name");
        memcpy(data->lessons[i].subject, startptr+10, endptr-startptr-13);
        data->lessons[i].subject[endptr-startptr-13] = 0;
        //person_name
        startptr = endptr;
        endptr = strstr(startptr, "}");
        memcpy(data->lessons[i].person_name, startptr+14, endptr-startptr-15);
        data->lessons[i].person_name[endptr-startptr-15] = 0;
    }

    return 8;
}

static char * convert_from_utf(const guint16 utf_symbol, char *converted_data) {
    if( utf_symbol <= 0x7F ) {
        *converted_data = (unsigned char)utf_symbol;
    }else if( utf_symbol <= 0x7FF) {
        guint16 tdata = 0x6 << 13 ;
        tdata |= (utf_symbol & (0x1F << 6)) << 2;
        tdata |= 0x2 << 6;
        tdata |= utf_symbol & 0x3F;
        *((guint16 *)converted_data) = ((tdata & 0xFF00) >> 8) | ((tdata & 0xFF) << 8);
    } else {
        *converted_data = '?';
    }
    return converted_data;
}

typedef char (*COMMAND_HANDLER)(const char* command, char* answer);

static char* date_time(const char *command, char *answer){
	time_t rawtime;
	time(&rawtime);
	struct tm *timeinfo = localtime(&rawtime);
	sprintf(answer, "Date and time: %s", asctime(timeinfo));
	return answer;
}

static char* version(const char *command, char *answer){
	strcpy(answer, "Версия: 0.2, билд от 15.09.2009");
	return answer;
}

static char* schedule(const char *command, char *answer){
		requests_schedule_count++;
		char *curcharptr = (char *)command;
        int groupNumber = 0;
        time_t date;
        time(&date);
        gboolean is_tomorrow = FALSE;
        while(curcharptr != 0) {
            if(strstr((const char *)curcharptr, " ") == 0)
                break;
            curcharptr = strstr((const char *)curcharptr, " ") + 1;
            if (*curcharptr >= '0' && *curcharptr <= '9') {
                groupNumber = atoi(curcharptr);
                curcharptr += sizeof(groupNumber);
            } else if((strstr((const char *)curcharptr, "tomorrow") || strstr((const char *)curcharptr, "на завтра"))&& !is_tomorrow) {
                date += 24 * 3600;
                curcharptr += (*curcharptr == 't') ? 8 : 9;
                is_tomorrow = TRUE;
            }
        }
		
		if(!groupNumber){
			strcpy(answer, "Пожалуйста, укажите номер группы.");
			return answer;
		}
		

        char buffer[5120], out[5120];
        get_schedule_json(groupNumber, &buffer[0], date);
        decode_utf_literals(&buffer[0], &out[0]);
        data data;
        int lessons = parse_json(out, &data);

        if (lessons == -1) {
            strcpy(answer, "Недопустимый номер группы");
        } else if (lessons == 0) {
            sprintf(answer, "%s, %d неделя<br>Группа: %d<br><br>", data.day, data.week_number, data.group);
            strcat(answer, "<br>Нет занятий");
        } else {
            sprintf(answer, "%s, %d неделя<br>Группа: %d<br><br>", data.day, data.week_number, data.group);
            sprintf(answer, "%s_______________________________<br>", answer);
            for (int i = 0; i < lessons - 1; i++) {
                if (i) {
                    sprintf(answer, "%s_______________________________<br>", answer);
                }
                sprintf(answer, "%s%s %s - %s<br>%s<br>", answer, data.lessons[i].time, data.lessons[i].place, data.lessons[i].person_name, data.lessons[i].subject);
                //sprintf(answer, "%s_______________________________<br>", answer);
            }
        }
		return answer;
}


static char* help(const char *command, char *answer){
	strcpy(answer, "Список доступных комманд:<br>"
                "ENG:<br>"
                " schedule %group_number% - выводит расписание для группы %group_number%<br>"
                " schedule %group_number% tomorrow - выводит расписание на завтра для группы %group_number%<br>"
                " time - выводит текущее время и дату<br>"
                " date - выводит текущее время и дату<br>"
                " version - выводит информацию о версии<br>"
                " help - выводит это сообщение<br>"
                "РУС:<br>"
                " расписание %номер_группы% - выводит расписание для группы %номер_группы%<br>"
                " расписание %номер_группы% на завтра - выводит расписание на завтра для группы %номер_группы%<br>"
                " время - выводит текущее время и дату<br>"
                " дата - выводит текущее время и дату<br>"
                " версия - выводит информацию о версии<br>"
                " помощь - выводит это сообщение<br>"
                );
	return answer;
}

static char* stat(const char *command, char *answer){
	sprintf(answer, "С запуска %d запросов расписания", requests_schedule_count);
	return answer;
}


GHashTable *commands_table = NULL;

static void init_commands_table()
{
	if(commands_table)
		return;
	
	COMMANDS_TABLE_INIT();
	
	COMMANDS_TABLE_ENTRY("date", date_time);
	COMMANDS_TABLE_ENTRY("дата", date_time);
	COMMANDS_TABLE_ENTRY("time", date_time);
	COMMANDS_TABLE_ENTRY("время", date_time);
	
	COMMANDS_TABLE_ENTRY("version", version);
	COMMANDS_TABLE_ENTRY("версия", version);
	
	COMMANDS_TABLE_ENTRY("schedule", schedule);
	COMMANDS_TABLE_ENTRY("расписание", schedule);
	
	COMMANDS_TABLE_ENTRY("help", help);
	COMMANDS_TABLE_ENTRY("помощь", help);
	
	COMMANDS_TABLE_ENTRY("stat", stat);
	COMMANDS_TABLE_ENTRY("стат", stat);
}

static char * get_answer(const char *command, char *answer) {

	char* cmd_end = strstr(command, " ");
	
	//security threat, possible buffer overflow
	char cmd[30];
	int cmdLength;
	
	cmdLength = (cmd_end) ? (cmd_end - command) : strlen(command);
	cmdLength = min(cmdLength, 29);
	
	
	
	memcpy(cmd, command, cmdLength);
	
	cmd[cmdLength] = 0;
	const char* normalized_command = purple_normalize_nocase(globalAccount, cmd);
	
	gpointer func = g_hash_table_lookup(commands_table, normalized_command);
	if(!func){
		strcpy(answer, "Неизвестная команда!");
	} else {
		((COMMAND_HANDLER)func)(command, answer);
	}
	
    return answer;
}

void show_usage(){
	printf("\nUsage: main --icq.login <uin> --icq.pass <pass>\n");
}

void quit_handler(int sig){
	printf("Ctrl+C received. See u!\n");
	exit(0);
}

void init_account(){
	globalAccount = purple_account_new(&uin[0], icqPluginInfo->id);
	
    purple_account_set_password(globalAccount, &password[0]);
    purple_account_set_enabled(globalAccount, UI_ID, TRUE);

    purple_account_set_bool(globalAccount, "authorization", FALSE);
	
	presence = purple_presence_new_for_account(globalAccount);
	
	INIT_STATUS(STATUS_AVAILABLE, PURPLE_STATUS_AVAILABLE);
	INIT_STATUS(STATUS_OFFLINE, PURPLE_STATUS_OFFLINE);
	INIT_STATUS(STATUS_AWAY, PURPLE_STATUS_AWAY);	
	
	//purple_presence_set_status_active(presence, purple_status_get_id(awayStatus), TRUE);
	
	purple_account_set_status(globalAccount, STATUS_ID(STATUS_AVAILABLE), TRUE, 0);	
}

/*void destroy_account(){
	//TODO
	//purple_account_destroy(globalAccount);
}*/

int main(int argc, char *argv[]) {
    //char uin[10] = "573869459";
    //char uin[10] = "595266840";
    

    const int optIcgLogin = 1;
    const int optIcgPass = 2;
	const int optLogPath = 3;
	
	int required_options = optIcgLogin | optIcgPass;

    static struct option long_options[] = {
        {"icq.login", required_argument, 0, optIcgLogin},
        {"icq.pass", required_argument, 0, optIcgPass},
		{"log.path", required_argument, 0, optLogPath} 
    };

    int option_index = 0;

    int c = -1;

    while ((c = getopt_long(argc, argv, "", long_options, &option_index)) != -1) {
        switch (c) {
            case optIcgLogin:
                strcpy(&uin[0], optarg);
				required_options &= ~optIcgLogin;
                break;
            case optIcgPass:
                strcpy(&password[0], optarg);
				required_options &= ~optIcgPass;
                break;
			case optLogPath:
				log_init(optarg);
				break;
        }
    }
	
	if(required_options){
		show_usage();
		exit(0);
	}
	

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);

    signal(SIGCHLD, SIG_IGN);
	signal(SIGINT, quit_handler);

    init_libpurple();

    printf("libpurple initialized.\n");

    icqPlugin = purple_plugins_find_with_name("ICQ");
    icqPluginInfo = icqPlugin->info;

	init_account();
	
    connect_to_signals();
	init_commands_table();
    g_main_loop_run(loop);

	log_uninit();
    return 0;
}
