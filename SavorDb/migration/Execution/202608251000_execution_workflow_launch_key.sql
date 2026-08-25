ALTER TABLE exec_workflow_instance ADD COLUMN launch_key TEXT;

CREATE UNIQUE INDEX ux_exec_workflow_instance_launch_key
ON exec_workflow_instance(launch_key)
WHERE launch_key IS NOT NULL;
