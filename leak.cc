#include <iostream>

using namespace std;

class A
{
public:
	A();
	~A();

private:
	char* foo;
};

A::A()
{
	foo = new char[100];
	cout << "Allocated A\n";
}

A::~A()
{
	cout << "Destroyed A\n";
}

int main()
{
	A test;
	return 0;
}
