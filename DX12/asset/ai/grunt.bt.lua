-- Sample Behavior Tree: simple combat grunt.
-- Order of priority: attack > chase > patrol/idle.
-- Designer adds new branches by extending the children list.

return {
    type = "Selector",
    name = "Root",
    children = {
        {
            type = "Sequence",
            name = "Attack if in range",
            children = {
                { type = "Condition", func = "InRange",
                  params = { target = "TargetEntity", range = 2.0 } },
                { type = "Action", func = "LogTick",
                  params = { message = "attack" } },
            },
        },
        {
            type = "Sequence",
            name = "Chase visible target",
            children = {
                { type = "BlackboardCondition", key = "TargetEntity", op = "is_set" },
                { type = "Action", func = "MoveToTarget",
                  params = { target = "TargetEntity", speed = 5.0,
                             arriveDistance = 1.5 } },
            },
        },
        {
            type = "Action", func = "Idle",
        },
    },
}
