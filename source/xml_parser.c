/*
 * Copyright 2010-2019 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <aws/auth/private/xml_parser.h>

#include <aws/common/array_list.h>

struct cb_stack_data {
    aws_xml_parser_on_node_encountered_fn *cb;
    void *user_data;
};

int aws_xml_parser_init(struct aws_xml_parser *parser, struct aws_allocator *allocator, struct aws_byte_cursor *doc) {
    AWS_ZERO_STRUCT(*parser);
    parser->allocator = allocator;
    parser->doc = *doc;

    aws_array_list_init_dynamic(&parser->cb_stack, allocator, 4, sizeof(struct cb_stack_data));
    return AWS_OP_SUCCESS;
}

void aws_xml_parser_clean_up(struct aws_xml_parser *parser) {
    if (parser->allocator) {
        aws_array_list_clean_up(&parser->cb_stack);
        AWS_ZERO_STRUCT(parser);
    }
}

int s_node_next_sibling(struct aws_xml_parser *parser);

static bool s_trim_quotes_fn(uint8_t value) {
    return value == '\"';
}

/* load the node declaration line, parsing node name and attributes. */
static int s_load_node_decl(
    struct aws_xml_parser *parser,
    struct aws_byte_cursor *decl_body,
    struct aws_xml_node *node) {
    struct aws_array_list splits;
    AWS_ZERO_STRUCT(splits);

    AWS_ZERO_ARRAY(parser->split_scratch);
    aws_array_list_init_static(
        &splits, parser->split_scratch, AWS_ARRAY_SIZE(parser->split_scratch), sizeof(struct aws_byte_cursor));

    /* split by space, first split will be the node name, everything after will be attribute=value pairs. For now
     * we limit to 10 attributes, if this is exceeded we consider it invalid document. */
    if (aws_byte_cursor_split_on_char(decl_body, ' ', &splits)) {
        return aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
    }

    size_t splits_count = aws_array_list_length(&splits);

    if (splits_count < 1) {
        return aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
    }

    aws_array_list_get_at(&splits, &node->name, 0);

    AWS_ZERO_ARRAY(parser->attributes);
    if (splits.length > 1) {
        aws_array_list_init_static(
            &node->attributes,
            parser->attributes,
            AWS_ARRAY_SIZE(parser->attributes),
            sizeof(struct aws_xml_attribute));

        for (size_t i = 1; i < splits.length; ++i) {
            struct aws_byte_cursor attribute_pair;
            AWS_ZERO_STRUCT(attribute_pair);
            aws_array_list_get_at(&splits, &attribute_pair, i);

            struct aws_byte_cursor att_val_pair[2];
            AWS_ZERO_ARRAY(att_val_pair);
            struct aws_array_list att_val_pair_lst;
            AWS_ZERO_STRUCT(att_val_pair_lst);
            aws_array_list_init_static(&att_val_pair_lst, att_val_pair, 2, sizeof(struct aws_byte_cursor));

            if (!aws_byte_cursor_split_on_char(&attribute_pair, '=', &att_val_pair_lst)) {
                struct aws_xml_attribute attribute = {
                    .name = att_val_pair[0],
                    .value = aws_byte_cursor_trim_pred(&att_val_pair[1], s_trim_quotes_fn),
                };
                aws_array_list_push_back(&node->attributes, &attribute);
            }
        }
    }

    return AWS_OP_SUCCESS;
}

int aws_xml_parser_parse(
    struct aws_xml_parser *parser,
    aws_xml_parser_on_node_encountered_fn *on_node_encountered,
    void *user_data) {

    aws_array_list_clear(&parser->cb_stack);

    /* burn everything that precedes the actual xml nodes. */
    while (parser->doc.len) {
        uint8_t *start = memchr(parser->doc.ptr, '<', parser->doc.len);
        if (!start) {
            return aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
        }

        uint8_t *location = memchr(parser->doc.ptr, '>', parser->doc.len);

        if (!location) {
            return aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
        }

        aws_byte_cursor_advance(&parser->doc, start - parser->doc.ptr);
        /* if these are preamble statements, burn them. otherwise don't seek at all
         * and assume it's just the doc with no preamble statements. */
        if (*(parser->doc.ptr + 1) == '?' || *(parser->doc.ptr + 1) == '!') {
            /* nobody cares about the preamble */
            size_t advance = location - parser->doc.ptr + 1;
            aws_byte_cursor_advance(&parser->doc, advance);
        } else {
            break;
        }
    }

    /* now we should be at the start of the actual document. */
    struct cb_stack_data stack_data = {
        .cb = on_node_encountered,
        .user_data = user_data,
    };

    AWS_FATAL_ASSERT(!aws_array_list_push_back(&parser->cb_stack, &stack_data));
    return s_node_next_sibling(parser);
}

int s_advance_to_closing_tag(
    struct aws_xml_parser *parser,
    struct aws_xml_node *node,
    struct aws_byte_cursor *out_body) {
    /* currently the max node name is 256 characters. This is arbitrary, but should be enough
     * for our uses. If we ever generalize this, we'll have to come back and rethink this. */
    uint8_t name_close[259] = {0};

    struct aws_byte_buf cmp_buf = aws_byte_buf_from_empty_array(name_close, sizeof(name_close));

    size_t closing_name_len = node->name.len + 3;

    if (closing_name_len > node->doc_at_body.len) {
        return aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
    }

    if (sizeof(name_close) < closing_name_len) {
        return aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
    }

    struct aws_byte_cursor open_bracket = aws_byte_cursor_from_c_str("</");
    struct aws_byte_cursor close_bracket = aws_byte_cursor_from_c_str(">");

    aws_byte_buf_append(&cmp_buf, &open_bracket);
    aws_byte_buf_append(&cmp_buf, &node->name);
    aws_byte_buf_append(&cmp_buf, &close_bracket);

    struct aws_byte_cursor to_find = aws_byte_cursor_from_buf(&cmp_buf);
    struct aws_byte_cursor find_result;
    AWS_ZERO_STRUCT(find_result);
    if (aws_byte_cursor_find_exact(&node->doc_at_body, &to_find, &find_result)) {
        return aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
    }

    size_t len = find_result.ptr - node->doc_at_body.ptr;
    aws_byte_cursor_advance(&parser->doc, len + cmp_buf.len);

    if (out_body) {
        *out_body = aws_byte_cursor_from_array(node->doc_at_body.ptr, len);
    }
    return AWS_OP_SUCCESS;
}

int aws_xml_node_as_body(struct aws_xml_parser *parser, struct aws_xml_node *node, struct aws_byte_cursor *out_body) {
    return s_advance_to_closing_tag(parser, node, out_body);
}

int aws_xml_node_traverse(
    struct aws_xml_parser *parser,
    struct aws_xml_node *node,
    aws_xml_parser_on_node_encountered_fn *on_node_encountered,
    void *user_data) {
    struct cb_stack_data stack_data = {
        .cb = on_node_encountered,
        .user_data = user_data,
    };

    if (aws_array_list_push_back(&parser->cb_stack, &stack_data)) {
        return AWS_OP_ERR;
    }

    /* look for the next node at the current level. do this until we encounter the parent node's
     * closing tag. */
    while (true) {
        uint8_t *next_location = memchr(parser->doc.ptr, '<', parser->doc.len);

        if (!next_location) {
            return aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
        }

        uint8_t *end_location = memchr(parser->doc.ptr, '>', parser->doc.len);

        if (!end_location) {
            return aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
        }

        bool parent_closed = false;

        if (*(next_location + 1) == '/') {
            parent_closed = true;
        }

        size_t node_name_len = end_location - next_location;

        aws_byte_cursor_advance(&parser->doc, end_location - parser->doc.ptr + 1);

        if (parent_closed) {
            break;
        }

        struct aws_byte_cursor decl_body = aws_byte_cursor_from_array(next_location + 1, node_name_len - 1);

        struct aws_xml_node next_node = {
            .doc_at_body = parser->doc,
        };

        if (s_load_node_decl(parser, &decl_body, &next_node)) {
            return AWS_OP_ERR;
        }

        if (!on_node_encountered(parser, &next_node, user_data)) {
            return AWS_OP_SUCCESS;
        }

        if (s_advance_to_closing_tag(parser, node, NULL)) {
            return AWS_OP_ERR;
        }
    }
    aws_array_list_pop_back(&parser->cb_stack);
    return AWS_OP_SUCCESS;
}

/* advance the parser to the next sibling node.*/
int s_node_next_sibling(struct aws_xml_parser *parser) {
    uint8_t *next_location = memchr(parser->doc.ptr, '<', parser->doc.len);

    if (!next_location) {
        return AWS_OP_SUCCESS;
    }

    aws_byte_cursor_advance(&parser->doc, next_location - parser->doc.ptr);
    uint8_t *end_location = memchr(parser->doc.ptr, '>', parser->doc.len);

    if (!end_location) {
        return aws_raise_error(AWS_ERROR_MALFORMED_INPUT_STRING);
    }

    size_t node_name_len = end_location - next_location;
    aws_byte_cursor_advance(&parser->doc, end_location - parser->doc.ptr + 1);

    struct aws_byte_cursor node_decl_body = aws_byte_cursor_from_array(next_location + 1, node_name_len - 1);

    struct aws_xml_node sibling_node = {
        .doc_at_body = parser->doc,
    };

    if (s_load_node_decl(parser, &node_decl_body, &sibling_node)) {
        return AWS_OP_ERR;
    }

    struct cb_stack_data stack_data;
    AWS_ZERO_STRUCT(stack_data);
    aws_array_list_back(&parser->cb_stack, &stack_data);
    stack_data.cb(parser, &sibling_node, stack_data.user_data);

    return AWS_OP_SUCCESS;
}
