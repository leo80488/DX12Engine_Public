#pragma once
#include<queue>
#include<bitset>
#include<optional>
class Keyboard {
	friend class Window;
public:
	class Event {
	public:
		enum class Type
		{
			Press,
			Release,
			Invalid
		};
	private:
		Type type;
		unsigned char code;
	public:
		Event() :type(Type::Invalid),code(0u) {}
		Event(Type type, unsigned char code) :type(type), code(code) {}
		bool IsPress() const
		{
			return type == Type::Press;
		}
		bool IsRelease() const
		{
			return type == Type::Release;
		}
		bool IsValid() const
		{
			return type != Type::Invalid;
		}
		unsigned char GetCode() const
		{
			return code;
		}
	};
public:
	static Keyboard& GetInstance()
	{
		static Keyboard instance;
		return instance;
	}
	Keyboard(const Keyboard&) = delete;
	Keyboard& operator=(const Keyboard&) = delete;
	//Key event
	bool KeyIsPressed(unsigned char keycode)const;
	bool KeyIsTriggered(unsigned char keycode)const; // Check whether the key was triggered this frame (not pressed -> pressed)
	std::optional<Event> Readkey();
	bool KeyIsEmpty();
	void FlushKey();
	void EndOfFrame(); // Called at the end of each frame to update last-frame key states
	// char event
	char ReadChar();
	bool CharIsempty() const;
	void FlushChar();
	void Flush();
	//autorepeat control
	void EnableAutorepeat();
	void DisableAutorepeat();
	bool AutorepeatIsEnabled() const;

	void OnKeyPressed(unsigned char keycode);
	void OnKeyReleased(unsigned char keycode);
private:
	Keyboard() = default;
	void OnChar(char character);
	void ClearState();

	template<typename T>
	static void TrimBuffer(std::queue<T>& buffer);
private:
	static constexpr unsigned int nKeys = 256u;
	static constexpr unsigned int bufferSize = 16u;
	bool autorepeatEnabled = false;
	std::bitset<nKeys> keystates;
	std::bitset<nKeys> keystatesLastFrame; // Last frame's key states, used for trigger detection
	std::queue<Event> keybuffer;
	std::queue<char> charbuffer;


};


