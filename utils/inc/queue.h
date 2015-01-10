/*
 * queue.h
 *
 *  Created on: Jan 7, 2015
 *      Author: felix
 */

#ifndef INC_QUEUE_H_
#define INC_QUEUE_H_

#include "common.h"

#include <strings.h>

#define CACHE_LINE_COUNT_QUEUE_NODE 128U

static const unsigned int ITEM_COUNT = CACHE_LINE_COUNT_QUEUE_NODE * CACHE_LINE_SIZE / sizeof(void*);

template<typename Item>
class Queue;

template<typename Item>
class QueueNode {
private:
	QueueNode<Item>* volatile next;
	volatile unsigned int head;
	volatile unsigned int tail;
	Item* volatile items[ITEM_COUNT] __attribute__ ((aligned (CACHE_LINE_SIZE)));

public:
	inline QueueNode();
	inline virtual ~QueueNode();

	inline bool push(Item* item);
	inline bool pop(Item*& item);

	inline bool isFull() const;
	inline void reset();

	friend class Queue<Item> ;
};

template<typename Item>
class Queue {
private:
	//Memory memory;
	QueueNode<Item>* volatile dummy;
	QueueNode<Item>* volatile tail;

	QueueNode<Item>* volatile dummyReserved;
	QueueNode<Item>* volatile tailReserved;

public:
	Queue();
	virtual ~Queue();

	void push(Item* item);
	Item* pop();

private:
	void pushReserved(QueueNode<Item>*& item);
	QueueNode<Item>* popReserved();
};

template<typename Item>
QueueNode<Item>::QueueNode() :
		next(0), head(0), tail(0) {
	for (unsigned int i = 0; i < ITEM_COUNT; ++i) items[i] = 0;
	__sync_synchronize();
}

template<typename Item>
QueueNode<Item>::~QueueNode() {
	reset();
	__sync_synchronize();
}

template<typename Item>
bool QueueNode<Item>::push(Item* item) {
	while (true) {
		unsigned int tailOld = this->tail;
		if (ITEM_COUNT * 2 <= tailOld + 1) {
			return false;
		}

		if (0 == tailOld % 2 && __sync_bool_compare_and_swap(&this->tail, tailOld, tailOld + 1)) {
			items[tailOld / 2] = item;
			__sync_fetch_and_add(&this->tail, 1);
			return true;
		}
	}
}

template<typename Item>
bool QueueNode<Item>::pop(Item*& item) {
	while (true) {
		unsigned int headOld = this->head;
		if (this->tail <= headOld + 1) {
			__sync_synchronize();
			headOld = this->head;
			if (this->tail <= headOld + 1) {
				item = 0;
				return false;
			}
		}

		if (__sync_bool_compare_and_swap(&this->head, headOld, headOld + 2)) {
			item = items[headOld / 2];
			return true;
		}
	}
}

template<typename Item>
bool QueueNode<Item>::isFull() const {
	return ITEM_COUNT * 2 <= this->tail;
}

template<typename Item>
void QueueNode<Item>::reset() {
	for (unsigned int i = 0; i < ITEM_COUNT; ++i) items[i] = 0;
	this->tail = 0;
	this->head = 0;
	this->next = 0;
}

template<typename Item>
Queue<Item>::Queue() :
		dummy(new QueueNode<Item>()), tail(dummy), dummyReserved(new QueueNode<Item>()), tailReserved(this->dummyReserved) {
	__sync_synchronize();
}

template<typename Item>
Queue<Item>::~Queue() {
	while (this->pop())
		;
	this->tail = 0;
	delete this->dummy;
	this->dummy = 0;

	this->tailReserved = 0;
	while(true) {
		QueueNode<Item>* dummyReserved = this->dummyReserved;
		if (!dummyReserved) {
			__sync_synchronize();
			dummyReserved = this->dummyReserved;
			if (!dummyReserved) {
				break;
			}
		}

		__sync_bool_compare_and_swap(&this->dummyReserved, dummyReserved, dummyReserved->next);
		dummyReserved->next = 0;
		delete dummyReserved;
		dummyReserved = 0;
	}

	__sync_synchronize();
}

template<typename Item>
void Queue<Item>::push(Item* item) {
	QueueNode<Item>* tailNew = 0;
	while (true) {
		QueueNode<Item>* tailOld = this->tail;
		while (tailOld->next) {
			__sync_bool_compare_and_swap(&this->tail, tailOld, tailOld->next);
			tailOld = this->tail;
		}

		if (dummy != tailOld && tailOld->push(item)) {
			if (tailNew)
				this->pushReserved(tailNew);
			return;
		}

		if (!tailNew) {
			tailNew = this->popReserved();
			tailNew->push(item);
		}
		if (__sync_bool_compare_and_swap(&tailOld->next, 0, tailNew)) {
			__sync_bool_compare_and_swap(&this->tail, tailOld, tailNew);
			return;
		}
	}
}

template<typename Item>
Item* Queue<Item>::pop() {
	while (true) {
		QueueNode<Item>* dummy = this->dummy;
		QueueNode<Item>* headOld = dummy->next;
		if (headOld == 0) {
			__sync_synchronize();
			dummy = this->dummy;
			headOld = dummy->next;
			if (headOld == 0)
				return 0;
		}

		Item* item = 0;
		if (headOld->pop(item)) {
			return item;
		}

		if (!headOld->isFull()) {
			__sync_synchronize();
			if (!headOld->isFull())
				return 0;
		}

		if (headOld->pop(item)) {
			return item;
		}

		if (__sync_bool_compare_and_swap(&this->dummy, dummy, headOld))
			this->pushReserved(dummy);
	}
}

template<typename Item>
void Queue<Item>::pushReserved(QueueNode<Item>*& tailNew) {
	tailNew->next = 0;
	//tailNew->reset();

	while (true) {
		QueueNode<Item>* tailOld = this->tailReserved;
		while (tailOld->next) {
			__sync_bool_compare_and_swap(&this->tailReserved, tailOld, tailOld->next);
			tailOld = this->tailReserved;
		}

		if (__sync_bool_compare_and_swap(&tailOld->next, 0, tailNew)) {
			__sync_bool_compare_and_swap(&this->tailReserved, tailOld, tailNew);
			tailNew = 0;
			return;
		}
	}
}

template<typename Item>
QueueNode<Item>* Queue<Item>::popReserved() {
	while (true) {
		QueueNode<Item>* dummyReserved = this->dummyReserved;
		QueueNode<Item>* headOld = dummyReserved->next;
		if (headOld == 0) {
			__sync_synchronize();
			dummyReserved = this->dummyReserved;
			headOld = dummyReserved->next;
			if (headOld == 0)
				return new QueueNode<Item>();
		}

		if (__sync_bool_compare_and_swap(&this->dummyReserved, dummyReserved, headOld)) {
			dummyReserved->reset();
			return dummyReserved;
		}
	}
}

#endif /* INC_QUEUE_H_ */
