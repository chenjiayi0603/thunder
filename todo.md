
考虑：
MysqlStep改为继承step；
去掉StepState；
~~去掉 CoroutineState~~（已完成，见 docs/StepCo20-coroutine-migration.md）
等逐项迁到 StepCo20

Step去掉下面的成员：
Step* m_pNextStep = nullptr;
std::set<uint32> m_setNextStepSeq;
std::set<uint32> m_setPreStepSeq;
