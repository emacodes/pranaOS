/*
 * Copyright (c) 2021, Ashwin-Paudel, Krisna Pranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// includes
#include <base/String.h>
#include <libcore/ArgsParser.h> // this libraries will be added soon
#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv) 
{
    if (pledge("stdio", nullptr) < 0) {
        perror("pledge");
        return 1;
    }

    Vector<const char*> values;
    bool no_trailing_newline = false;

    Core::ArgsParser args_parser;
    args_parser.add_option(no_trailing_newline, "Do not output a trailed newline", nullptr, 'n');
    args_parser.add_positional_argument(values, "Values to print out", "string", Core::ArgsParser::Required::No);
    args_parser.parse(argc, argv);

    out("{}", String::join(' ', values));
    if (!no_trailing_newline)
        outln();
    return 0;
}