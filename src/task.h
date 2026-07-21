#ifndef NANOBOT_TASK_H
#define NANOBOT_TASK_H

/* Persistent goal/task board for multi-tool agent runs.
 * Tools: task_plan, task_start, task_step_done, task_done, task_block, task_status.
 * State: $NANOBOT_HOME/tasks/active.json
 */

/* OpenAI tools fragment (leading comma + objects, or empty). Malloc'd. */
char *ng_task_openai_tools_fragment(void);

/* Dispatch task_* tools. Returns malloc'd tool result text, or NULL if not a task tool. */
char *ng_task_try_tool(const char *name, const char *args_json);

/* 1 if active task exists and status is planned|active (not done/blocked). */
int ng_task_is_open(void);

/* Malloc'd multi-line reminder for the model (or NULL if no open task). */
char *ng_task_reminder_text(void);

/* Soft: max extra agent turns when a task is open (beyond normal max_turns). */
int ng_task_extra_turns(void);
/* Hard ceiling for one agent_run (normal + extras). */
int ng_task_hard_max_turns(void);

#endif
