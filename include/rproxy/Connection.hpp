#pragma once

class Connection {
public:
	virtual ~Connection() = default;
	virtual void process_client() = 0;
};
