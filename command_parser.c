#include <glib-2.0/glib.h>
#include <stdio.h>
#include <regex.h>
#include <string.h>
#include <command_parser.h>

#define LANG_RU 0x1
#define LANG_EN 0x2
#define LANG_ALL 0xF

typedef struct {
    char pattern[255];
    char lang;
    void *action;
    GSList *childs; //list of *tree_node
}tree_node;

tree_node * get_node_child(tree_node *node, char *data) {
    regex_t regex;
    tree_node *current;
    int i;
    int length = g_slist_length(node->childs);
    for(i = 0; i < length; i++) {
        current = (tree_node *)g_slist_nth(node->childs, i);
        regcomp(&regex, &(current->pattern), REG_ICASE);
        if( regexec(&regex, data, (size_t) 0, NULL, 0) == 0 ) {
            regfree(&regex);
            return current;
        }
        regfree(&regex);
    }
    return NULL;
}

tree_node *create_tree_node(tree_node *node, char *pattern, char lang, void *action, GSList *childs) {
    if(strlen(pattern) > 254) {
        return NULL;
    }
    strcpy(&(node->pattern[0]), pattern);
    node->lang = lang;
    node->action = action;
    node->childs = childs;
    return node;
}

//Debug purposes only!
int start() {
    tree_node root;
    create_tree_node(&root, "hello", LANG_ALL, NULL, NULL);

    printf("%s", root.pattern);
    return 0;
}

