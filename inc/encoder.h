#ifndef ENCODER_H_
#define ENCODER_H_

/**
 * @brief 初始化 QDEC 旋转编码器驱动。
 *
 * 校准 QDEC 外设，使能传感器并启动周期性采样工作队列。
 *
 * @retval 0 成功，负值为错误码。
 */
int encoder_init(void);

#endif /* ENCODER_H_ */
