UPDATE ui_workflow_instance
SET display_state=state
WHERE state IN ('COMPLETED','FAILED','CANCELED')
  AND display_state<>state;
