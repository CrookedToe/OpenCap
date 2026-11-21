execute_process(COMMAND "${GIT}" apply --no-index --reverse --check "${PATCH}"
    RESULT_VARIABLE already_applied OUTPUT_QUIET ERROR_QUIET)
if(NOT already_applied EQUAL 0)
    execute_process(COMMAND "${GIT}" apply --no-index "${PATCH}" COMMAND_ERROR_IS_FATAL ANY)
endif()
