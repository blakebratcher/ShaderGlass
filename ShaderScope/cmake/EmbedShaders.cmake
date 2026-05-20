function(embed_blob VAR FILE OUTSTR)
    file(READ ${FILE} HEX HEX)
    string(REGEX MATCHALL "[0-9a-f][0-9a-f]" BYTES ${HEX})
    set(BODY "")
    set(I 0)
    foreach(B ${BYTES})
        string(APPEND BODY "0x${B},")
        math(EXPR I "${I} + 1")
        if(${I} EQUAL 16)
            string(APPEND BODY "\n")
            set(I 0)
        endif()
    endforeach()
    set(${OUTSTR}
        "static const unsigned char ${VAR}[] = {\n${BODY}\n};\nstatic const unsigned long ${VAR}_len = sizeof(${VAR});\n"
        PARENT_SCOPE)
endfunction()

embed_blob(g_passthrough_vert_spv ${VERT_SPV} VERT_BLOB)
embed_blob(g_passthrough_frag_spv ${FRAG_SPV} FRAG_BLOB)

file(WRITE ${OUT}
"// Generated. Do not edit.
#pragma once
${VERT_BLOB}
${FRAG_BLOB}
")
