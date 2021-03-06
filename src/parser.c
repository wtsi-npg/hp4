#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include <jansson.h>

#include "debug.h"
#include "parser.h"
#include "pipe.h"
#include "strutil.h"

int append_edge_to_array(struct p4_edge_array **pea, struct p4_edge *pe) {
    if ((*pea) == NULL) {
        (*pea) = malloc(sizeof(*(*pea)));
        if ((*pea) == NULL) {
            REPORT_ERRORF("%s", strerror(errno));
            return -1;
        }
        (*pea)->edges = NULL;
        (*pea)->length = 0u;
    }
    if ((*pea)->length == 0u) {
        (*pea)->edges = malloc(sizeof(*(*pea)->edges));
        if ((*pea)->edges == NULL) {
            REPORT_ERRORF("%s", strerror(errno));
            return -1;
        }
        (*pea)->edges[0] = pe;
        (*pea)->length = 1;
    }
    else {
        (*pea)->length++;
        struct p4_edge **newmem = realloc((*pea)->edges,
                ((*pea)->length) * sizeof(*(*pea)->edges));
        if (newmem == NULL) {
            REPORT_ERRORF("%s", strerror(errno));
            return -1;
        }
        (*pea)->edges = newmem;
        (*pea)->edges[(*pea)->length - 1] = pe;
    }
    return 0;
}

int parse_p4_edge(json_t *edge, struct p4_edge *parsed_edge) {
    json_incref(edge);
    if (!json_is_object(edge)) {
        REPORT_ERROR("Attempted to parse an edge, but it was not a JSON object");
        json_decref(edge);
        return -1;
    }

    json_t *json_id;
    json_t *json_from;
    json_t *json_to;

    parsed_edge->bytes_spliced = 0l;

    if ((json_id = json_object_get(edge, "id"))) {
        parsed_edge->id = malloc((json_string_length(json_id) + 1) * sizeof(char));
        if (parsed_edge->id == NULL) {
            REPORT_ERRORF("%s", strerror(errno));
            json_decref(edge);
            return -1;
        }
        strcpy(parsed_edge->id, json_string_value(json_id));
    }
    else {
        parsed_edge->id = NULL;
    }

    if ((json_from = json_object_get(edge, "from"))) {
        const char *from_ro = json_string_value(json_from);
        char **from_parsed = parse_edge_string(from_ro);
        if (from_parsed != NULL) {
            parsed_edge->from = from_parsed[0];
            parsed_edge->from_port = from_parsed[1];
            free(from_parsed);
        }
        else {
            REPORT_ERRORF("Failed to parse `from` field in edge %s. Multiple ports?\n",
                    parsed_edge->id);
            parsed_edge->from = NULL;
            parsed_edge->from_port = NULL;
            json_decref(edge);
            return -1;
        }
    }
    else {
        parsed_edge->from = NULL;
        parsed_edge->from_port = NULL;
    }

    if ((json_to = json_object_get(edge, "to"))) {
        const char *to_ro = json_string_value(json_to);
        char **to_parsed = parse_edge_string(to_ro);
        if (to_parsed != NULL) {
            parsed_edge->to = to_parsed[0];
            parsed_edge->to_port = to_parsed[1];
            free(to_parsed);
        }
        else {
            REPORT_ERRORF("Failed to parse `to` field in edge %s. Multiple ports?\n",
                    parsed_edge->id);
            parsed_edge->to = NULL;
            parsed_edge->to_port = NULL;
            json_decref(edge);
            return -1;
        }
    }
    else {
        parsed_edge->to = NULL;
        parsed_edge->to_port = NULL;
    }

    json_decref(edge);
    return 0;
}

void free_p4_edge(struct p4_edge *pe) {
    if (pe != NULL) {
        free(pe->id);
        free(pe->from);
        free(pe->from_port);
        free(pe->to);
        free(pe->to_port);
        free(pe);
    }
}

void free_p4_edge_array(struct p4_edge_array *edges) {
    if (edges != NULL) {
        for (size_t i = 0u; i < edges->length; i++) {
            free_p4_edge(edges->edges[i]);
        }
        free(edges->edges);
        free(edges);
    }
}

struct p4_edge_array *p4_edge_array_new(json_t *edges, size_t length) {
    json_incref(edges);
    if (!json_is_array(edges)) {
        REPORT_ERROR("Input json was not an array");
        json_decref(edges);
        return NULL;
    }

    struct p4_edge_array *edge_arr = malloc(sizeof(*edge_arr));
    if (edge_arr == NULL) {
        REPORT_ERRORF("%s", strerror(errno));
        json_decref(edges);
        return NULL;
    }
    edge_arr->length = length;
    edge_arr->edges = calloc(edge_arr->length, sizeof(*edge_arr->edges));
    if (edge_arr->edges == NULL) {
        REPORT_ERRORF("%s", strerror(errno));
        free(edge_arr);
        return NULL;
    }

    for (int i = 0; i < (int)edge_arr->length; i++) {
        edge_arr->edges[i] = calloc(1u, sizeof(**edge_arr->edges));
        if (edge_arr->edges[i] == NULL) {
            REPORT_ERRORF("%s", strerror(errno));
            for (int j = 0; j < i; i++) {
                free(edge_arr->edges[j]);
            }
            free(edge_arr->edges);
            free(edge_arr);
            return NULL;
        }

        if (parse_p4_edge(json_array_get(edges, i), edge_arr->edges[i]) < 0) {
            free_p4_edge_array(edge_arr);
            json_decref(edges);
            return NULL;
        }
    }

    json_decref(edges);
    return edge_arr;
}

void free_p4_node(struct p4_node *pn) {
    if (pn != NULL) {
        free(pn->id);
        free(pn->type);
        free(pn->subtype);
        free(pn->cmd);
        free(pn->name);

        pipe_array_free(pn->in_pipes);
        pipe_array_free(pn->out_pipes);

        if (pn->listening_edges != NULL)
            free(pn->listening_edges->edges);
        free(pn->listening_edges);
        free(pn);
    }
}

void free_p4_node_array(struct p4_node_array *nodes) {
    if (nodes != NULL) {
        for (size_t i = 0u; i < nodes->length; i++) {
            free_p4_node(nodes->nodes[i]);
        }
        free(nodes->nodes);
        free(nodes);
    }
}

struct p4_node *find_node_by_id(struct p4_file *pf, const char *id) {
    for (int i = 0; i < (int)pf->nodes->length; i++) {
        struct p4_node *pn = p4_file_get_node(pf, i);
        if (strncmp(pn->id, id, strlen(id) + 1) == 0) {
            return pn;
        }
    }
    return NULL;
}

struct p4_node *find_node_by_pid(struct p4_file *pf, pid_t pid) {
    for (int i = 0; i < (int)pf->nodes->length; i++) {
        struct p4_node *pn = p4_file_get_node(pf, i);
        if (pid == pn->pid) {
            return pn;
        }
    }
    return NULL;
}

struct p4_edge *find_edge_by_id(struct p4_file *pf, const char *edge_id) {
    for (int i = 0; i < (int)pf->edges->length; i++) {
        struct p4_edge *pe = p4_file_get_edge(pf, i);
        if (strncmp(pe->id, edge_id, strlen(edge_id) + 1) == 0) {
            return pe;
        }
    }
    return NULL;
}

struct p4_node *get_node(struct p4_node_array *nodes, int idx) {
    if (idx < 0 || (unsigned int)idx >= nodes->length) {
        return NULL;
    }
    return nodes->nodes[idx];
}

struct p4_node *p4_file_get_node(struct p4_file *pf, int idx) {
    return get_node(pf->nodes, idx);
}

struct p4_edge *get_edge(struct p4_edge_array *edges, int idx) {
    if (idx < 0 || (unsigned int)idx >= edges->length) {
        return NULL;
    }
    return edges->edges[idx];
}

struct p4_edge *p4_file_get_edge(struct p4_file *pf, int idx) {
    return get_edge(pf->edges, idx);
}

struct p4_node *find_from_node_by_edge_id(struct p4_file *pf, const char *edge_id) {
    struct p4_edge *pe = find_edge_by_id(pf, edge_id);
    if (pe == NULL) {
        /* No edge with that id */
        return NULL;
    }
    return find_node_by_id(pf, pe->from);
}

struct p4_node *find_to_node_by_edge_id(struct p4_file *pf, const char *edge_id) {
    struct p4_edge *pe = find_edge_by_id(pf, edge_id);
    if (pe == NULL) {
        /* No edge with that id */
        return NULL;
    }
    return find_node_by_id(pf, pe->to);
}

int parse_p4_node(json_t *node, struct p4_node *parsed_node) {
    json_incref(node);
    if (!json_is_object(node)) {
        json_decref(node);
        return -1;
    }

    json_t *json_id;
    json_t *json_type;
    json_t *json_subtype;
    json_t *json_cmd;
    json_t *json_name;

    if ((json_id = json_object_get(node, "id"))) {
        parsed_node->id = malloc((json_string_length(json_id)+1) * sizeof(char));
        if (parsed_node->id == NULL) {
            REPORT_ERRORF("%s", strerror(errno));
            json_decref(node);
            return -1;
        }
        strcpy(parsed_node->id, json_string_value(json_id));
    }
    else {
        parsed_node->id = NULL;
    }
    if ((json_type = json_object_get(node, "type"))) {
        parsed_node->type = malloc((json_string_length(json_type)+1) * sizeof(char));
        if (parsed_node->type == NULL) {
            REPORT_ERRORF("%s", strerror(errno));
            free(parsed_node->id);
            json_decref(node);
            return -1;
        }
        strcpy(parsed_node->type, json_string_value(json_type));
    }
    else {
        parsed_node->type = NULL;
    }
    if ((json_subtype = json_object_get(node, "subtype"))) {
        parsed_node->subtype = malloc((json_string_length(json_subtype)+1) * sizeof(char));
        if (parsed_node->subtype == NULL) {
            REPORT_ERRORF("%s", strerror(errno));
            free(parsed_node->type);
            free(parsed_node->id);
            json_decref(node);
            return -1;
        }
        strcpy(parsed_node->subtype, json_string_value(json_subtype));
    }
    else {
        parsed_node->subtype = NULL;
    }
    if ((json_cmd = json_object_get(node, "cmd"))) {
        parsed_node->cmd = malloc((json_string_length(json_cmd)+1) * sizeof(char));
        if (parsed_node->cmd == NULL) {
            REPORT_ERRORF("%s", strerror(errno));
            free(parsed_node->subtype);
            free(parsed_node->type);
            free(parsed_node->id);
            json_decref(node);
            return -1;
        }
        strcpy(parsed_node->cmd, json_string_value(json_cmd));
    }
    else {
        parsed_node->cmd = NULL;
    }
    if ((json_name = json_object_get(node, "name"))) {
        parsed_node->name = malloc((json_string_length(json_name)+1) * sizeof(char));
        if (parsed_node->name == NULL) {
            REPORT_ERRORF("%s", strerror(errno));
            free(parsed_node->cmd);
            free(parsed_node->subtype);
            free(parsed_node->type);
            free(parsed_node->id);
            json_decref(node);
            return -1;
        }
        strcpy(parsed_node->name, json_string_value(json_name));
    }
    else {
        parsed_node->name = NULL;
    }

    parsed_node->in_pipes = pipe_array_new();
    parsed_node->out_pipes = pipe_array_new();
    if (parsed_node->in_pipes == NULL || parsed_node->out_pipes == NULL) {
        json_decref(node);
        return -1;
    }

    parsed_node->writable_events = event_array_new();
    if (parsed_node->writable_events == NULL) {
        json_decref(node);
        return -1;
    }

    json_decref(node);
    return 0;
}

struct p4_node_array *p4_node_array_new(json_t *nodes, size_t length) {
    json_incref(nodes);
    if (!json_is_array(nodes)) {
        REPORT_ERROR("Input json was not an array");
        json_decref(nodes);
        return NULL;
    }

    struct p4_node_array *node_arr = malloc(sizeof(*node_arr));
    if (node_arr == NULL) {
        REPORT_ERRORF("%s", strerror(errno));
        return NULL;
    }
    node_arr->length = length;
    node_arr->nodes = calloc(node_arr->length, sizeof(*node_arr->nodes));
    if (node_arr->nodes == NULL) {
        REPORT_ERRORF("%s", strerror(errno));
        free(node_arr);
        return NULL;
    }

    for (int i = 0; i < (int)node_arr->length; i++) {
        node_arr->nodes[i] = calloc(1u, sizeof(**node_arr->nodes));
        if (node_arr->nodes[i] == NULL) {
            REPORT_ERRORF("%s", strerror(errno));
            for (int j = 0; j < i; i++) {
                free(node_arr->nodes[j]);
            }
            free(node_arr->nodes);
            free(node_arr);
            return NULL;
        }

        if (parse_p4_node(json_array_get(nodes, i), node_arr->nodes[i]) < 0) {
            REPORT_ERROR("Failed to parse node");

            free(node_arr);
            json_decref(nodes);
            return NULL;
        }
    }
    json_decref(nodes);
    return node_arr;
}

struct p4_file *p4_file_new(const char *filename) {
    json_error_t error;
    json_t *root = json_load_file(filename, 0, &error);
    if (root == NULL) {
        fprintf(stderr, "parsing json failed at %d: %s\n", error.line, error.text);
        json_decref(root);
        return NULL;
    }
    if (!json_is_object(root)) {
        REPORT_ERROR("Root is not an object");
        json_decref(root);
        return NULL;
    }

    json_t *nodes = json_object_get(root, "nodes");
    if (!json_is_array(nodes)) {
        REPORT_ERROR("nodes is not an array");
        json_decref(root);
        return NULL;
    }
    json_t *edges = json_object_get(root, "edges");
    if (!json_is_array(edges)) {
        REPORT_ERROR("error: edges is not an array");
        json_decref(root);
        return NULL;
    }

    struct p4_file *pf = malloc(sizeof(*pf));
    if (pf == NULL) {
        REPORT_ERRORF("%s", strerror(errno));
        return NULL;
    }

    pf->nodes = NULL;
    pf->edges = NULL;

    pf->edges = p4_edge_array_new(edges, json_array_size(edges));
    if (pf->edges == NULL) {
        free_p4_file(pf);
        json_decref(root);
        return NULL;
    }
    pf->nodes = p4_node_array_new(nodes, json_array_size(nodes));
    if (pf->nodes == NULL) {
        free_p4_file(pf);
        json_decref(root);
        return NULL;
    }

    json_decref(root);
    return pf;
}

void free_p4_file(struct p4_file *pf) {
    if (pf != NULL) {
        free_p4_node_array(pf->nodes);
        free_p4_edge_array(pf->edges);
        free(pf);
    }
}
